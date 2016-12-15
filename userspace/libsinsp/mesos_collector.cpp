/** collector

A full notice with attributions is provided along with this source code.

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License version 2 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program; if not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

* In addition, as a special exception, the copyright holders give
* permission to link the code of portions of this program with the
* OpenSSL library under certain conditions as described in each
* individual source file, and distribute linked combinations
* including the two.
* You must obey the GNU General Public License in all respects
* for all of the code used other than OpenSSL.  If you modify
* file(s) with this exception, you may extend this exception to your
* version of the file(s), but you are not obligated to do so.  If you
* do not wish to do so, delete this exception statement from your
* version.
*/

//
// mesos_collector.cpp
//

#ifdef HAS_CAPTURE

#include "sinsp.h"
#include "sinsp_int.h"
#include "mesos_collector.h"
#include "mesos_http.h"
#include <string.h>
#include <sstream>
#include <unistd.h>


mesos_collector::mesos_collector(bool do_loop, long timeout_ms):
	m_nfds(0),
	m_loop(do_loop),
	m_timeout_ms(timeout_ms),
	m_stopped(false)
{
	clear();
}

mesos_collector::~mesos_collector()
{
}

void mesos_collector::clear()
{
	FD_ZERO(&m_errfd);
	FD_ZERO(&m_infd);
}

void mesos_collector::add(std::shared_ptr<mesos_http> handler)
{
	int sockfd = handler->get_socket(m_timeout_ms);

	FD_SET(sockfd, &m_errfd);
	FD_SET(sockfd, &m_infd);
	if(sockfd > m_nfds)
	{
		m_nfds = sockfd;
	}
	m_sockets[sockfd] = handler;
}

bool mesos_collector::has(std::shared_ptr<mesos_http> handler)
{
	for(const auto& http : m_sockets)
	{
		if(http.second == handler)
		{
			return true;
		}
	}
	return false;
}

bool mesos_collector::remove(std::shared_ptr<mesos_http> handler)
{
	for(socket_map_t::iterator it = m_sockets.begin(); it != m_sockets.end(); ++it)
	{
		if(it->second == handler)
		{
			remove(it);
			return true;
		}
	}
	return false;
}

mesos_collector::socket_map_t::iterator& mesos_collector::remove(socket_map_t::iterator& it)
{
	if(it != m_sockets.end())
	{
		m_sockets.erase(it++);
	}
	m_nfds = 0;
	for(const auto& sock : m_sockets)
	{
		if(sock.first > m_nfds)
		{
			m_nfds = sock.first;
		}
	}
	return it;
}

void mesos_collector::remove_all()
{
	clear();
	m_sockets.clear();
	m_nfds = 0;
}

bool mesos_collector::is_active() const
{
	return subscription_count() > 0;
}

bool mesos_collector::is_healthy(int expected_count) const
{
	return subscription_count() >= expected_count;
}

int mesos_collector::subscription_count() const
{
	return m_sockets.size();
}

void mesos_collector::get_data()
{
	try
	{
		struct timeval tv;
		int res;
		m_stopped = false;
		while(!m_stopped)
		{
			tv.tv_sec  = m_loop ? m_timeout_ms / 1000 : 0;
			tv.tv_usec = m_loop ? (m_timeout_ms % 1000) * 1000 : 0;
			{
				if(m_sockets.size())
				{
					g_logger.log("Mesos collector number of sockets: " + std::to_string(m_sockets.size()), sinsp_logger::SEV_DEBUG);
					res = select(m_nfds + 1, &m_infd, NULL, &m_errfd, &tv);
					if(res < 0) // error
					{
						std::string err = std::string("Mesos collector select error, removing all sockets (") + strerror(errno) + ")";
						g_logger.log(err, sinsp_logger::SEV_ERROR);
						g_json_error_log.log("", err, sinsp_utils::get_current_time_ns(), "mesos-collector-get-data");
						remove_all();
					}
					else // data or idle
					{
						for(socket_map_t::iterator it = m_sockets.begin(); it != m_sockets.end();)
						{
							if(FD_ISSET(it->first, &m_infd))
							{
								if(it->second && !it->second->on_data())
								{
									if(errno != EAGAIN)
									{
										std::string fid = it->second->get_framework_id();
										if(!fid.empty())
										{
											std::string errstr = "Mesos collector data handling error, removing Marathon socket for framework [" + fid + ']';
											g_logger.log(errstr, sinsp_logger::SEV_ERROR);
											g_json_error_log.log("", errstr, sinsp_utils::get_current_time_ns(), "mesos-collector-get-data");
										}
										else
										{
											std::string errstr = "Mesos collector data handling error, removing Mesos state socket.";
											g_logger.log(errstr, sinsp_logger::SEV_ERROR);
											g_json_error_log.log("", errstr, sinsp_utils::get_current_time_ns(), "mesos-collector-get-data");
										}
										remove(it);
										continue;
									}
								}
							}
							else
							{
								FD_SET(it->first, &m_infd);
							}

							if(FD_ISSET(it->first, &m_errfd))
							{
								if(errno != EAGAIN)
								{
									std::string errstr = std::string("Mesos collector select errfd: ") + strerror(errno);
									g_logger.log(errstr, sinsp_logger::SEV_ERROR);
									g_json_error_log.log("", errstr, sinsp_utils::get_current_time_ns(), "mesos-collector-get-data");
									std::string fid;
									if(it->second)
									{
										it->second->on_error(errstr, true);
										fid = it->second->get_framework_id();
									}
									if(!fid.empty())
									{
										std::string errstr = "Mesos collector socket error, removing Marathon socket for framework [" + fid + ']';
										g_logger.log(errstr, sinsp_logger::SEV_ERROR);
										g_json_error_log.log("", errstr, sinsp_utils::get_current_time_ns(), "mesos-collector-get-data");
									}
									else
									{
										std::string errstr = "Mesos collector socket error, removing Mesos state socket.";
										g_logger.log(errstr, sinsp_logger::SEV_ERROR);
										g_json_error_log.log("", errstr, sinsp_utils::get_current_time_ns(), "mesos-collector-get-data");
									}
									remove(it);
									continue;
								}
							}
							else
							{
								FD_SET(it->first, &m_errfd);
							}
							++it;
						}
					}
				}
				else
				{
					std::string errstr = "Mesos collector is empty. Stopping.";
					g_logger.log(errstr, sinsp_logger::SEV_ERROR);
					g_json_error_log.log("", errstr, sinsp_utils::get_current_time_ns(), "mesos-collector-get-data");
					m_stopped = true;
					return;
				}
			}
			if(!m_loop)
			{
				break;
			}
		}
	}
	catch(std::exception& ex)
	{
		std::string errstr = std::string("Mesos collector error: ") + ex.what();
		g_logger.log(errstr, sinsp_logger::SEV_ERROR);
		g_json_error_log.log("", errstr, sinsp_utils::get_current_time_ns(), "mesos-collector-get-data");
		remove_all();
		m_stopped = true;
	}
}


#endif // HAS_CAPTURE
