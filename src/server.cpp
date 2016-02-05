/*
 * Copyright 2015+ Evgeniy Polyakov <zbr@ioremap.net>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <elliptics/session.hpp>

#include <swarm/logger.hpp>

#include <thevoid/rapidjson/stringbuffer.h>
#include <thevoid/rapidjson/prettywriter.h>
#include <thevoid/rapidjson/document.h>

#include <thevoid/server.hpp>
#include <thevoid/stream.hpp>

#include <unistd.h>
#include <signal.h>

#define NLOG(level, a...) BH_LOG(logger(), level, ##a)
#define NLOG_ERROR(a...) NLOG(SWARM_LOG_ERROR, ##a)
#define NLOG_WARNING(a...) NLOG(SWARM_LOG_WARNING, ##a)
#define NLOG_INFO(a...) NLOG(SWARM_LOG_INFO, ##a)
#define NLOG_NOTICE(a...) NLOG(SWARM_LOG_NOTICE, ##a)
#define NLOG_DEBUG(a...) NLOG(SWARM_LOG_DEBUG, ##a)

using namespace ioremap;

class nulla_server : public thevoid::server<nulla_server>
{
public:
	virtual bool initialize(const rapidjson::Value &config) {
		if (!elliptics_init(config))
			return false;

		on<on_dash>(
			options::prefix_match("/dash"),
			options::methods("POST")
		);

		on<on_dash_stream>(
			options::prefix_match("/dash_stream"),
			options::methods("GET")
		);

		return true;
	}

	struct on_dash : public thevoid::simple_request_stream<nulla_server> {
		virtual void on_request(const thevoid::http_request &req, const boost::asio::const_buffer &buffer) {
			(void) buffer;
			(void) req;

			this->send_reply(thevoid::http_response::ok);
		}
	};

	struct on_dash_stream : public thevoid::simple_request_stream<nulla_server> {
		virtual void on_request(const thevoid::http_request &req, const boost::asio::const_buffer &buffer) {
			(void) buffer;
			(void) req;

			NLOG_INFO("url: %s: %s", req.url().to_human_readable().c_str(), __PRETTY_FUNCTION__);

			this->send_reply(thevoid::http_response::ok);
		}
	};

private:
	std::shared_ptr<elliptics::node> m_node;

	long m_read_timeout = 60;
	long m_write_timeout = 60;

	bool elliptics_init(const rapidjson::Value &config) {
		dnet_config node_config;
		memset(&node_config, 0, sizeof(node_config));

		if (!prepare_config(config, node_config)) {
			return false;
		}

		m_node.reset(new elliptics::node(std::move(swarm::logger(this->logger(), blackhole::log::attributes_t())), node_config));

		if (!prepare_node(config, *m_node)) {
			return false;
		}

		if (!prepare_session(config)) {
			return false;
		}

		return true;
	}

	bool prepare_config(const rapidjson::Value &config, dnet_config &node_config) {
		if (config.HasMember("io-thread-num")) {
			node_config.io_thread_num = config["io-thread-num"].GetInt();
		}
		if (config.HasMember("nonblocking-io-thread-num")) {
			node_config.nonblocking_io_thread_num = config["nonblocking-io-thread-num"].GetInt();
		}
		if (config.HasMember("net-thread-num")) {
			node_config.net_thread_num = config["net-thread-num"].GetInt();
		}

		return true;
	}

	bool prepare_node(const rapidjson::Value &config, elliptics::node &node) {
		if (!config.HasMember("remotes")) {
			NLOG_ERROR("\"application.remotes\" field is missed");
			return false;
		}

		std::vector<elliptics::address> remotes;

		auto &remotesArray = config["remotes"];
		for (auto it = remotesArray.Begin(), end = remotesArray.End(); it != end; ++it) {
				if (it->IsString()) {
					remotes.push_back(it->GetString());
				}
		}

		try {
			node.add_remote(remotes);
			elliptics::session session(node);

			if (!session.get_routes().size()) {
				NLOG_ERROR("Didn't add any remote node, exiting.");
				return false;
			}
		} catch (const std::exception &e) {
			NLOG_ERROR("Could not add any out of %d nodes.", remotes.size());
			return false;
		}

		return true;
	}

	bool prepare_session(const rapidjson::Value &config) {
		if (config.HasMember("read-timeout")) {
			auto &tm = config["read-timeout"];
			if (tm.IsInt())
				m_read_timeout = tm.GetInt();
		}

		if (config.HasMember("write-timeout")) {
			auto &tm = config["write-timeout"];
			if (tm.IsInt())
				m_write_timeout = tm.GetInt();
		}

		return true;
	}
};

int main(int argc, char **argv)
{
	if (argc == 1) {
		std::cerr << "Usage: " << argv[0] << " --config <config file>" << std::endl;
		return -1;
	}

	thevoid::register_signal_handler(SIGINT, thevoid::handle_stop_signal);
	thevoid::register_signal_handler(SIGTERM, thevoid::handle_stop_signal);
	thevoid::register_signal_handler(SIGHUP, thevoid::handle_reload_signal);
	thevoid::register_signal_handler(SIGUSR1, thevoid::handle_ignore_signal);
	thevoid::register_signal_handler(SIGUSR2, thevoid::handle_ignore_signal);

	thevoid::run_signal_thread();

	auto server = thevoid::create_server<nulla_server>();
	int err = server->run(argc, argv);

	thevoid::stop_signal_thread();

	return err;
}

