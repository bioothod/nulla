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

#include "nulla/asio.hpp"

#include <ebucket/bucket_processor.hpp>

#include <elliptics/session.hpp>

#include <swarm/logger.hpp>

#include <thevoid/rapidjson/stringbuffer.h>
#include <thevoid/rapidjson/prettywriter.h>
#include <thevoid/rapidjson/document.h>

#include <thevoid/server.hpp>
#include <thevoid/stream.hpp>

#include <unistd.h>
#include <signal.h>

#define NLOG(level, a...) BH_LOG(this->logger(), level, ##a)
#define NLOG_ERROR(a...) NLOG(SWARM_LOG_ERROR, ##a)
#define NLOG_WARNING(a...) NLOG(SWARM_LOG_WARNING, ##a)
#define NLOG_INFO(a...) NLOG(SWARM_LOG_INFO, ##a)
#define NLOG_NOTICE(a...) NLOG(SWARM_LOG_NOTICE, ##a)
#define NLOG_DEBUG(a...) NLOG(SWARM_LOG_DEBUG, ##a)

using namespace ioremap;

std::string get_bucket(const thevoid::http_request &req) {
	const auto &path = req.url().path_components();
	if (path.size() <= 2) {
		return "";
	}

	return path[1];
}

std::string get_key(const thevoid::http_request &req) {
	const auto &path = req.url().path_components();
	if (path.size() <= 2) {
		return "";
	}

	size_t prefix_size = 1 + path[0].size() + 1 + path[1].size() + 1;
	return req.url().path().substr(prefix_size);
}

template <typename Server, typename Stream>
class on_dash_manifest_base : public thevoid::simple_request_stream<Server>, public std::enable_shared_from_this<Stream> {
public:
	virtual void on_request(const thevoid::http_request &req, const boost::asio::const_buffer &buffer) {
		(void) buffer;

		if (!this->server()->check_bucket_key(req)) {
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		std::string bname = get_bucket(req);
		std::string key = get_key(req);

		NLOG_INFO("%s: bucket: %s, key: %s", __func__, bname, key);

		ebucket::bucket b;
		elliptics::error_info err = this->server()->bucket_processor()->find_bucket(bname, b);
		if (err) {
			NLOG_ERROR("url: %s: could not find bucket %s in bucket processor: %s [%d]",
					req.url().to_human_readable(), bname, err.message(), err.code());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		b->session().read_data(key, 0, 0).connect(std::bind(&on_dash_manifest_base::on_read,
				this->shared_from_this(), std::placeholders::_1, std::placeholders::_2));
	}

	virtual void on_error(const boost::system::error_code &error) {
		NLOG_ERROR("buffered-read: on_error: url: %s, error: %s",
				this->request().url().to_human_readable().c_str(), error.message());
	}

	void on_read(const elliptics::sync_read_result &result, const elliptics::error_info &error)
	{
		if (error) {
			NLOG_ERROR("buffered-read: %s: url: %s: error: %s",
					__func__, this->request().url().to_human_readable(), error.message());

			auto ec = boost::system::errc::make_error_code(static_cast<boost::system::errc::errc_t>(-error.code()));
			this->close(ec);
			return;
		}

		const elliptics::read_result_entry &entry = result[0];
		elliptics::data_pointer file = entry.file();

		NLOG_INFO("buffered-read: %s: url: %s, data-size: %d",
				__func__, this->request().url().to_human_readable(),
				file.size());

		thevoid::http_response reply;
		reply.set_code(swarm::http_response::ok);
		reply.headers().set_content_length(file.size());
		reply.headers().set("Access-Control-Allow-Credentials", "true");
		reply.headers().set("Access-Control-Allow-Origin", "*");

		this->send_headers(std::move(reply), std::move(file),
				std::bind(&on_dash_manifest_base::close, this->shared_from_this(), std::placeholders::_1));
	}
};

template <typename Server>
class on_dash_manifest : public on_dash_manifest_base<Server, on_dash_manifest<Server>>
{
public:
};

template <typename Server, typename Stream>
class on_dash_stream_base : public thevoid::simple_request_stream<Server>, public std::enable_shared_from_this<Stream> {
public:
	void on_request(const thevoid::http_request &req, const boost::asio::const_buffer &buffer) {
		(void) buffer;

		if (!this->server()->check_bucket_key(req)) {
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		// url format: http://host[:port]/get/bucket/key[?uri_parameters]
		// where bucket and key are mandatory parameters, bucket may not contain '/' symbol,
		// key may contain any symbol

		const auto &path = req.url().path_components();
		if (path.size() <= 2) {
			NLOG_ERROR("url: %s: invalid path, there must be at least 2 path components: /bucket/key",
					req.url().to_human_readable());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		std::string bname = get_bucket(req);
		std::string key = get_key(req);

		if (key.empty()) {
			NLOG_ERROR("url: %s: invalid path, there must be at least 2 path components and "
					"key should not be empty: /bucket/key",
					req.url().to_human_readable());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		bool init = req.url().query().has_item("init");

		NLOG_INFO("%s: bucket: %s, key: %s, init: %d", __func__, bname, key, init);

		ebucket::bucket b;
		elliptics::error_info err = this->server()->bucket_processor()->find_bucket(bname, b);
		if (err) {
			NLOG_ERROR("url: %s: could not find bucket %s in bucket processor: %s [%d]",
					req.url().to_human_readable(), bname, err.message(), err.code());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		elliptics::session s = b->session();

		if (init) {
			s.read_data(key, 0, 0).connect(std::bind(&on_dash_stream_base::on_read,
				this->shared_from_this(), std::placeholders::_1, std::placeholders::_2));
			return;
		}
	}

	virtual void on_error(const boost::system::error_code &error) {
		NLOG_ERROR("buffered-write: on_error: url: %s, error: %s",
				this->request().url().to_human_readable().c_str(), error.message());
	}

	void on_read(const elliptics::sync_read_result &result, const elliptics::error_info &error)
	{
		if (error) {
			NLOG_ERROR("buffered-get: %s: url: %s: error: %s",
					__func__, this->request().url().to_human_readable(), error.message());

			auto ec = boost::system::errc::make_error_code(static_cast<boost::system::errc::errc_t>(-error.code()));
			this->close(ec);
			return;
		}

		const elliptics::read_result_entry &entry = result[0];
		elliptics::data_pointer file = entry.file();

		thevoid::http_response reply;
		reply.set_code(swarm::http_response::ok);
		reply.headers().set_content_length(file.size());
		reply.headers().set("Access-Control-Allow-Credentials", "true");
		reply.headers().set("Access-Control-Allow-Origin", "*");

		this->send_headers(std::move(reply), std::move(file),
				std::bind(&on_dash_stream_base::close, this->shared_from_this(), std::placeholders::_1));
	}
};

template <typename Server>
class on_dash_stream : public on_dash_stream_base<Server, on_dash_stream<Server>>
{
public:
};



class nulla_server : public thevoid::server<nulla_server>
{
public:
	virtual bool initialize(const rapidjson::Value &config) {
		if (!elliptics_init(config))
			return false;

		on<on_dash_manifest<nulla_server>>(
			options::prefix_match("/dash_manifest"),
			options::methods("GET")
		);

		on<on_dash_stream<nulla_server>>(
			options::prefix_match("/dash_stream"),
			options::methods("GET")
		);

		return true;
	}

	bool check_bucket_key(const thevoid::http_request &req) {
		const auto &path = req.url().path_components();
		if (path.size() <= 2) {
			NLOG_ERROR("url: %s: invalid path, there must be at least 2 path components: /bucket/key",
					req.url().to_human_readable());
			return false;
		}

		size_t prefix_size = 1 + path[0].size() + 1 + path[1].size() + 1;
		if (req.url().path().size() - prefix_size == 0) {
			NLOG_ERROR("url: %s: invalid path, there must be at least 2 path components and "
					"key should not be empty: /bucket/key",
					req.url().to_human_readable());
			return false;
		}

		return true;
	}

	std::shared_ptr<ebucket::bucket_processor> bucket_processor() const {
		return m_bp;
	}

private:
	std::shared_ptr<elliptics::node> m_node;
	std::shared_ptr<ebucket::bucket_processor> m_bp;

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

		m_bp.reset(new ebucket::bucket_processor(m_node));

		if (!prepare_session(config)) {
			return false;
		}

		if (!prepare_buckets(config)) {
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

	bool prepare_buckets(const rapidjson::Value &config) {
		if (!config.HasMember("buckets")) {
			NLOG_ERROR("\"application.buckets\" field is missed");
			return false;
		}

		auto &buckets = config["buckets"];

		std::set<std::string> bnames;
		for (auto it = buckets.Begin(), end = buckets.End(); it != end; ++it) {
			if (it->IsString()) {
				bnames.insert(it->GetString());
			}
		}

		if (!config.HasMember("metadata-groups")) {
			NLOG_ERROR("\"application.metadata-groups\" field is missed");
			return false;
		}

		std::vector<int> mgroups;
		auto &groups_meta_array = config["metadata-groups"];
		for (auto it = groups_meta_array.Begin(), end = groups_meta_array.End(); it != end; ++it) {
			if (it->IsInt())
				mgroups.push_back(it->GetInt());
		}

		if (!m_bp->init(mgroups, std::vector<std::string>(bnames.begin(), bnames.end())))
			return false;

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

