/*
 * COPYRIGHT (C) 2017-2021, zhllxt
 *
 * author   : zhllxt
 * email    : 37792738@qq.com
 * 
 * Distributed under the GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
 * (See accompanying file LICENSE or see <http://www.gnu.org/licenses/>)
 */

#ifndef __ASIO2_MQTT_AOP_CONNECT_HPP__
#define __ASIO2_MQTT_AOP_CONNECT_HPP__

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
#pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <asio2/base/iopool.hpp>

#include <asio2/base/detail/function_traits.hpp>
#include <asio2/base/detail/util.hpp>

#include <asio2/mqtt/message.hpp>

namespace asio2::detail
{
	template<class caller_t, class args_t>
	class mqtt_aop_connect
	{
		friend caller_t;

	protected:
		// must be server
		template<class Message, class Response>
		inline bool _before_connect_callback(
			error_code& ec, std::shared_ptr<caller_t>& caller_ptr, caller_t* caller, mqtt::message& om,
			Message& msg, Response& rep)
		{
			detail::ignore_unused(ec, caller_ptr, caller, om, msg, rep);

			using message_type  [[maybe_unused]] = typename detail::remove_cvref_t<Message>;
			using response_type [[maybe_unused]] = typename detail::remove_cvref_t<Response>;

			if constexpr (caller_t::is_session())
			{
				// if started already and recvd connect message again, disconnect
				state_t expected = state_t::started;
				if (caller->state_.compare_exchange_strong(expected, state_t::started))
				{
					ec = mqtt::make_error_code(mqtt::error::malformed_packet);
					rep.set_send_flag(false);
					return false;
				}

				// A Server MAY allow a Client to supply a ClientId that has a length of zero bytes, 
				// however if it does so the Server MUST treat this as a special case and assign a 
				// unique ClientId to that Client. It MUST then process the CONNECT packet as if the
				// Client had provided that unique ClientId [MQTT-3.1.3-6].
				// If the Client supplies a zero-byte ClientId, the Client MUST also set CleanSession 
				// to 1[MQTT-3.1.3-7].
				// If the Client supplies a zero-byte ClientId with CleanSession set to 0, the Server 
				// MUST respond to the CONNECT Packet with a CONNACK return code 0x02 (Identifier rejected) 
				// and then close the Network Connection[MQTT-3.1.3-8].
				// If the Server rejects the ClientId it MUST respond to the CONNECT Packet with a CONNACK
				// return code 0x02 (Identifier rejected) and then close the Network Connection[MQTT-3.1.3-9].
				if (msg.client_id().empty() && msg.clean_session() == false)
				{
					ec = mqtt::make_error_code(mqtt::error::client_identifier_not_valid);

					if constexpr /**/ (std::is_same_v<response_type, mqtt::v3::connack>)
					{
						rep.reason_code(mqtt::v3::connect_reason_code::identifier_rejected);
					}
					else if constexpr (std::is_same_v<response_type, mqtt::v4::connack>)
					{
						rep.reason_code(mqtt::v4::connect_reason_code::identifier_rejected);
					}
					else if constexpr (std::is_same_v<response_type, mqtt::v5::connack>)
					{
						rep.reason_code(mqtt::error::client_identifier_not_valid);
					}
					else
					{
						ASIO2_ASSERT(false);
					}

					return false;
				}
				else
				{
					rep.reason_code(0);
				}

				// If a client with the same Client ID is already connected to the server, the "older" client
				// must be disconnected by the server before completing the CONNECT flow of the new client.

				// If CleanSession is set to 0, the Server MUST resume communications with the Client based on state
				// from the current Session (as identified by the Client identifier).
				// If there is no Session associated with the Client identifier the Server MUST create a new Session.
				// The Client and Server MUST store the Session after the Client and Server are disconnected [MQTT-3.1.2-4].
				// After the disconnection of a Session that had CleanSession set to 0, the Server MUST store further
				// QoS 1 and QoS 2 messages that match any subscriptions that the client had at the time of disconnection
				// as part of the Session state [MQTT-3.1.2-5]. It MAY also store QoS 0 messages that meet the same criteria.
				// If CleanSession is set to 1, the Client and Server MUST discard any previous Session and start a new one.
				// This Session lasts as long as the Network Connection.State data associated with this Session MUST NOT be
				// reused in any subsequent Session[MQTT-3.1.2-6].

				// If a CONNECT packet is received with Clean Start is set to 1, the Client and Server MUST discard any
				// existing Session and start a new Session [MQTT-3.1.2-4]. Consequently, the Session Present flag in
				// CONNACK is always set to 0 if Clean Start is set to 1.
				// If a CONNECT packet is received with Clean Start set to 0 and there is a Session associated with the
				// Client Identifier, the Server MUST resume communications with the Client based on state from the existing
				// Session[MQTT-3.1.2-5].If a CONNECT packet is received with Clean Start set to 0 and there is no Session
				// associated with the Client Identifier, the Server MUST create a new Session[MQTT-3.1.2-6].

				// assign a unique ClientId to that Client.
				if (msg.client_id().empty())
				{
					msg.client_id(std::to_string(reinterpret_cast<std::size_t>(caller)));
				}

				caller->connect_message_ = msg;

				asio2_unique_lock lock{ caller->get_mutex() };

				auto iter = caller->mqtt_sessions_.find(caller->client_id());

				// If the Server accepts a connection with Clean Start set to 1, the Server MUST set Session
				// Present to 0 in the CONNACK packet in addition to setting a 0x00 (Success) Reason Code in
				// the CONNACK packet [MQTT-3.2.2-2].
				if (msg.clean_session())
					rep.session_present(false);
				// If the Server accepts a connection with Clean Start set to 0 and the Server has Session 
				// State for the ClientID, it MUST set Session Present to 1 in the CONNACK packet, otherwise
				// it MUST set Session Present to 0 in the CONNACK packet. In both cases it MUST set a 0x00
				// (Success) Reason Code in the CONNACK packet [MQTT-3.2.2-3].
				else
					rep.session_present(iter != caller->mqtt_sessions_.end());

				if (iter == caller->mqtt_sessions_.end())
				{
					iter = caller->mqtt_sessions_.emplace(caller->client_id(), caller_ptr).first;
				}
				else
				{
					auto& session_ptr = iter->second;

					if (session_ptr->is_started())
					{
						// send will message
						if (session_ptr->connect_message_.index() != std::variant_npos)
						{
							auto f = [caller_ptr, caller](auto& conn) mutable
							{
								if (!conn.will_flag())
									return;

								// note : why v5 ?
								mqtt::v5::publish pub;
								pub.qos(conn.will_qos());
								pub.retain(conn.will_retain());
								pub.topic_name(conn.will_topic());
								pub.payload(conn.will_payload());

								caller->push_event(
								[caller_ptr, caller, pub = std::move(pub)]
								(event_queue_guard<caller_t> g) mutable
								{
									detail::ignore_unused(g);

									caller->_multicast_publish(caller_ptr, caller, std::move(pub), std::string{});
								});
							};

							if /**/ (std::holds_alternative<mqtt::v3::connect>(session_ptr->connect_message_.base()))
							{
								mqtt::v3::connect* p = session_ptr->connect_message_.template get_if<mqtt::v3::connect>();
								f(*p);
							}
							else if (std::holds_alternative<mqtt::v4::connect>(session_ptr->connect_message_.base()))
							{
								mqtt::v4::connect* p = session_ptr->connect_message_.template get_if<mqtt::v4::connect>();
								f(*p);
							}
							else if (std::holds_alternative<mqtt::v5::connect>(session_ptr->connect_message_.base()))
							{
								mqtt::v5::connect* p = session_ptr->connect_message_.template get_if<mqtt::v5::connect>();
								f(*p);
							}
						}

						// disconnect session
						session_ptr->stop();

						// 
						bool clean_session = false;

						if /**/ (std::holds_alternative<mqtt::v3::connect>(session_ptr->connect_message_.base()))
						{
							clean_session = session_ptr->connect_message_.template get_if<mqtt::v3::connect>()->clean_session();
						}
						else if (std::holds_alternative<mqtt::v4::connect>(session_ptr->connect_message_.base()))
						{
							clean_session = session_ptr->connect_message_.template get_if<mqtt::v4::connect>()->clean_session();
						}
						else if (std::holds_alternative<mqtt::v5::connect>(session_ptr->connect_message_.base()))
						{
							clean_session = session_ptr->connect_message_.template get_if<mqtt::v5::connect>()->clean_session();
						}

						if (clean_session)
						{
						}
						else
						{
						}

						if (msg.clean_session())
						{

						}
						else
						{
							// copy session state from old session to new session
						}
					}
					else
					{

					}

					// replace old session to new session
					session_ptr = caller_ptr;
				}

				return true;
			}
			else
			{
				return true;
			}
		}

		// must be server
		inline void _before_user_callback_impl(
			error_code& ec, std::shared_ptr<caller_t>& caller_ptr, caller_t* caller, mqtt::message& om,
			mqtt::v3::connect& msg, mqtt::v3::connack& rep)
		{
			if (!_before_connect_callback(ec, caller_ptr, caller, om, msg, rep))
				return;
		}

		// must be server
		inline void _before_user_callback_impl(
			error_code& ec, std::shared_ptr<caller_t>& caller_ptr, caller_t* caller, mqtt::message& om,
			mqtt::v4::connect& msg, mqtt::v4::connack& rep)
		{
			if (!_before_connect_callback(ec, caller_ptr, caller, om, msg, rep))
				return;
		}

		// must be server
		inline void _before_user_callback_impl(
			error_code& ec, std::shared_ptr<caller_t>& caller_ptr, caller_t* caller, mqtt::message& om,
			mqtt::v5::connect& msg, mqtt::v5::connack& rep)
		{
			if (!_before_connect_callback(ec, caller_ptr, caller, om, msg, rep))
				return;
		}

		// must be server
		inline void _before_user_callback_impl(
			error_code& ec, std::shared_ptr<caller_t>& caller_ptr, caller_t* caller, mqtt::message& om,
			mqtt::v5::connect& msg, mqtt::v5::auth& rep)
		{
			detail::ignore_unused(ec, caller_ptr, caller, om, msg, rep);

			//if constexpr (caller_t::is_session())
			//{
			//	// if started already and recvd connect message again, disconnect
			//	state_t expected = state_t::started;
			//	if (caller->state_.compare_exchange_strong(expected, state_t::started))
			//	{
			//		ec = mqtt::make_error_code(mqtt::error::malformed_packet);
			//		rep.set_send_flag(false);
			//		return;
			//	}

			//	caller->connect_message_ = msg;

			//	asio2_unique_lock lock{ caller->get_mutex() };

			//	auto iter = caller->mqtt_sessions_.find(msg.client_id());
			//	if (iter != caller->mqtt_sessions_.end())
			//	{

			//	}
			//}
			//else
			//{
			//	std::ignore = true;
			//}
		}

		inline void _after_user_callback_impl(
			error_code& ec, std::shared_ptr<caller_t>& caller_ptr, caller_t* caller, mqtt::message& om,
			mqtt::v3::connect& msg, mqtt::v3::connack& rep)
		{
			detail::ignore_unused(ec, caller_ptr, caller, om, msg, rep);
			// If a client with the same Client ID is already connected to the server, the "older" client
			// must be disconnected by the server before completing the CONNECT flow of the new client.
			switch(rep.reason_code())
			{
			case mqtt::v3::connect_reason_code::success                       : ec = mqtt::make_error_code(mqtt::error::success                     ); break;
			case mqtt::v3::connect_reason_code::unacceptable_protocol_version : ec = mqtt::make_error_code(mqtt::error::unsupported_protocol_version); break;
			case mqtt::v3::connect_reason_code::identifier_rejected			  : ec = mqtt::make_error_code(mqtt::error::client_identifier_not_valid ); break;
			case mqtt::v3::connect_reason_code::server_unavailable			  : ec = mqtt::make_error_code(mqtt::error::server_unavailable          ); break;
			case mqtt::v3::connect_reason_code::bad_user_name_or_password	  : ec = mqtt::make_error_code(mqtt::error::bad_user_name_or_password   ); break;
			case mqtt::v3::connect_reason_code::not_authorized				  : ec = mqtt::make_error_code(mqtt::error::not_authorized              ); break;
			default                                                           : ec = mqtt::make_error_code(mqtt::error::malformed_packet            ); break;
			}
		}

		inline void _after_user_callback_impl(
			error_code& ec, std::shared_ptr<caller_t>& caller_ptr, caller_t* caller, mqtt::message& om,
			mqtt::v4::connect& msg, mqtt::v4::connack& rep)
		{
			detail::ignore_unused(ec, caller_ptr, caller, om, msg, rep);
			switch(rep.reason_code())
			{
			case mqtt::v4::connect_reason_code::success						  : ec = mqtt::make_error_code(mqtt::error::success                     ); break;
			case mqtt::v4::connect_reason_code::unacceptable_protocol_version : ec = mqtt::make_error_code(mqtt::error::unsupported_protocol_version); break;
			case mqtt::v4::connect_reason_code::identifier_rejected			  : ec = mqtt::make_error_code(mqtt::error::client_identifier_not_valid ); break;
			case mqtt::v4::connect_reason_code::server_unavailable			  : ec = mqtt::make_error_code(mqtt::error::server_unavailable          ); break;
			case mqtt::v4::connect_reason_code::bad_user_name_or_password	  : ec = mqtt::make_error_code(mqtt::error::bad_user_name_or_password   ); break;
			case mqtt::v4::connect_reason_code::not_authorized				  : ec = mqtt::make_error_code(mqtt::error::not_authorized              ); break;
			default                                                           : ec = mqtt::make_error_code(mqtt::error::malformed_packet            ); break;
			}
		}

		inline void _after_user_callback_impl(
			error_code& ec, std::shared_ptr<caller_t>& caller_ptr, caller_t* caller, mqtt::message& om,
			mqtt::v5::connect& msg, mqtt::v5::connack& rep)
		{
			detail::ignore_unused(ec, caller_ptr, caller, om, msg, rep);

			ec = mqtt::make_error_code(static_cast<mqtt::error>(rep.reason_code()));

			if (!ec)
			{
				if (rep.properties().has<mqtt::v5::topic_alias_maximum>() == false)
					rep.properties().add(mqtt::v5::topic_alias_maximum{ caller->topic_alias_maximum() });
			}
		}

		inline void _after_user_callback_impl(
			error_code& ec, std::shared_ptr<caller_t>& caller_ptr, caller_t* caller, mqtt::message& om,
			mqtt::v5::connect& msg, mqtt::v5::auth& rep)
		{
			detail::ignore_unused(ec, caller_ptr, caller, om, msg, rep);
		}
	};
}

#endif // !__ASIO2_MQTT_AOP_CONNECT_HPP__
