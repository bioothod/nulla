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
#include "nulla/mpd_generator.hpp"
#include "nulla/iso_reader.hpp"
#include "nulla/iso_writer.hpp"
#include "nulla/playlist.hpp"

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

#include <atomic>

using namespace ioremap;

static void nulla_log(const swarm::logger &logger, swarm::log_level level, const char *fmt, ...) __attribute__ ((format(printf, 3, 4)));

static void nulla_log(const swarm::logger &logger, swarm::log_level level, const char *fmt, ...)
{
	auto record = logger.open_record(level);
	if (record) {
		va_list args;
		va_start(args, fmt);

		char buffer[2048];
		const size_t buffer_size = sizeof(buffer);

		vsnprintf(buffer, buffer_size, fmt, args);

		buffer[buffer_size - 1] = '\0';

		size_t len = strlen(buffer);
		while (len > 0 && buffer[len - 1] == '\n')
			buffer[--len] = '\0';

		try {
			record.attributes.insert(blackhole::keyword::message() = buffer);
		} catch (...) {
		}
		logger.push(std::move(record));
		va_end(args);
	}
}

#define NLOG(level, a...) nulla_log(this->logger(), level, ##a)
#define NLOG_ERROR(a...) NLOG(SWARM_LOG_ERROR, ##a)
#define NLOG_WARNING(a...) NLOG(SWARM_LOG_WARNING, ##a)
#define NLOG_INFO(a...) NLOG(SWARM_LOG_INFO, ##a)
#define NLOG_NOTICE(a...) NLOG(SWARM_LOG_NOTICE, ##a)
#define NLOG_DEBUG(a...) NLOG(SWARM_LOG_DEBUG, ##a)

template <typename Server, typename Stream>
class on_dash_manifest_base : public thevoid::simple_request_stream<Server>, public std::enable_shared_from_this<Stream> {
public:
	virtual void on_request(const thevoid::http_request &req, const boost::asio::const_buffer &buffer) {
		m_playlist = std::shared_ptr<nulla::raw_playlist>(new nulla::raw_playlist());

		elliptics::error_info err = parse_manifest_request(buffer);
		if (err) {
			NLOG_ERROR("url: %s: could not parse manifest request: %.*s, error: %s [%d]",
					req.url().to_human_readable().c_str(),
					(int)boost::asio::buffer_size(buffer),
					boost::asio::buffer_cast<const unsigned char*>(buffer),
					err.message().c_str(), err.code());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		for (auto repr_it = m_playlist->repr.begin(), repr_it_end = m_playlist->repr.end();
				repr_it != repr_it_end;
				++repr_it) {

			size_t track_position = 0;
			for (auto track_it = repr_it->second.tracks.begin(), track_it_end = repr_it->second.tracks.end();
					track_it != track_it_end; ++track_it) {
				ebucket::bucket b;
				err = this->server()->bucket_processor()->find_bucket(track_it->bucket, b);
				if (err) {
					NLOG_ERROR("url: %s: could not find bucket %s in bucket processor: %s [%d]",
							req.url().to_human_readable().c_str(), track_it->bucket.c_str(),
							err.message().c_str(), err.code());

					this->send_reply(thevoid::http_response::bad_request);
					return;
				}

				b->session().read_data(track_it->key + ".meta", 0, 0).connect(
					std::bind(&on_dash_manifest_base::on_read_meta,
						this->shared_from_this(), repr_it->first, track_position,
						std::placeholders::_1, std::placeholders::_2));

				++track_position;
			}
		}
	}

	virtual void on_error(const boost::system::error_code &error) {
		NLOG_ERROR("buffered-read: on_error: url: %s, error: %s",
			this->request().url().to_human_readable().c_str(), error.message().c_str());
	}

private:
	nulla::playlist_t m_playlist;

	void update_periods() {
		BOOST_FOREACH(nulla::period &p, m_playlist->periods) {
			BOOST_FOREACH(nulla::adaptation &a, p.adaptations) {
				BOOST_FOREACH(std::string &id, a.repr_ids) {
					auto it = m_playlist->repr.find(id);
					if (it == m_playlist->repr.end())
						continue;

					long dts_first_sample_offset = 0;
					long number = 0;

					nulla::representation &repr = it->second;
					repr.duration_msec = 0;
					BOOST_FOREACH(nulla::track_request &tr, repr.tracks) {
						const nulla::track &track = tr.track();

						tr.dts_start = tr.start_msec * track.media_timescale / 1000;
						tr.dts_first_sample_offset = dts_first_sample_offset;
						tr.start_number = number;

						repr.duration_msec += tr.duration_msec;
						number += (tr.duration_msec + 1000 * m_playlist->chunk_duration_sec - 1) / (1000 * m_playlist->chunk_duration_sec);

						const auto &ls = track.samples[track.samples.size() - 1];
						const auto &pls = track.samples[track.samples.size() - 2];

						long last_diff_dts = ls.dts - pls.dts;
						dts_first_sample_offset += ls.dts + last_diff_dts;

						
					}

					if (p.duration_msec == 0 || p.duration_msec < it->second.duration_msec)
						p.duration_msec = repr.duration_msec;
				}
			}
		}
	}

	void check_and_send_manifest() {
		if (m_playlist->meta_chunks_read == m_playlist->meta_chunks_requested) {
			update_periods();

			std::string cookie = this->server()->store_playlist(m_playlist);
			//m_playlist->base_url = "http://" + this->request().local_endpoint() + "/" + cookie + "/";
			m_playlist->base_url = "http://localhost:8080/stream/" + cookie + "/";

			send_manifest();
		}
	}

	void send_manifest() {
		std::string url = m_playlist->base_url + "manifest";

		thevoid::http_response reply;
		reply.set_code(swarm::http_response::ok);
		reply.headers().set_content_length(url.size());
		reply.headers().set("Access-Control-Allow-Credentials", "true");
		reply.headers().set("Access-Control-Allow-Origin", "*");

		this->send_headers(std::move(reply), std::move(url),
				std::bind(&on_dash_manifest_base::close, this->shared_from_this(), std::placeholders::_1));
	}

	void on_read_meta(const std::string &repr_id, size_t track_position,
			const elliptics::sync_read_result &result, const elliptics::error_info &error) {
		if (error) {
			NLOG_ERROR("meta-read: %s: url: %s: error: %s",
					__func__, this->request().url().to_human_readable().c_str(), error.message().c_str());

			this->send_reply(thevoid::http_response::service_unavailable);
			return;
		}

		// this callback can be invoked in parallel, but it does not change playlist's repr map,
		// two callbacks can update the same representation in parallel, but they update
		// different track requests and do not change mutual track alignment (like positions in std::vector or vector itself),
		// so we do not need to protect it
		auto repr_it = m_playlist->repr.find(repr_id);
		if (repr_it == m_playlist->repr.end()) {
			NLOG_ERROR("meta-read: %s: url: %s: could not find representation with id %s",
					__func__, this->request().url().to_human_readable().c_str(),
					repr_id.c_str());

			this->send_reply(thevoid::http_response::service_unavailable);
			return;
		}
		
		if (track_position >= repr_it->second.tracks.size()) {
			NLOG_ERROR("meta-read: %s: url: %s: repr_id: %s, track_position: %zd, tracks_size: %zd: position is out of band",
					__func__, this->request().url().to_human_readable().c_str(),
					repr_id.c_str(), track_position, repr_it->second.tracks.size());

			this->send_reply(thevoid::http_response::service_unavailable);
			return;
		}

		nulla::track_request &tr = repr_it->second.tracks[track_position];

		const elliptics::read_result_entry &entry = result[0];
		elliptics::error_info err = meta_unpack(entry.file(), tr);
		if (err) {
			NLOG_ERROR("meta-read: %s: url: %s: repr_id: %s, could not unpack metadata: %s",
					__func__, this->request().url().to_human_readable().c_str(),
					repr_id.c_str(), err.message().c_str());

			this->send_reply(thevoid::http_response::service_unavailable);
			return;
		}

		NLOG_INFO("meta-read: %s: url: %s, repr_id: %s, media-tracks: %zd",
			__func__, this->request().url().to_human_readable().c_str(),
			repr_id.c_str(), tr.media.tracks.size());

		++m_playlist->meta_chunks_read;
		check_and_send_manifest();
	}

	elliptics::error_info parse_manifest_request(const boost::asio::const_buffer &buffer) {
		// this is needed to put ending zero-byte, otherwise rapidjson parser will explode
		std::string data(const_cast<char *>(boost::asio::buffer_cast<const char*>(buffer)), boost::asio::buffer_size(buffer));

		rapidjson::Document doc;
		doc.Parse<0>(data.c_str());

		if (doc.HasParseError()) {
			return elliptics::create_error(-EINVAL, "could not parse document: %s, error offset: %zd", 
					doc.GetParseError(), doc.GetErrorOffset());
		}

		if (!doc.IsObject()) {
			return elliptics::create_error(-EINVAL, "document must be object, its type: %d", doc.GetType());
		}

		const auto &periods = ebucket::get_array(doc, "periods");
		if (!periods.IsArray()) {
			return elliptics::create_error(-EINVAL, "'periods' must be an array");
		}

		elliptics::error_info err;
		size_t idx = 0;
		for (auto it = periods.Begin(), it_end = periods.End(); it != it_end; ++it) {
			if (!it->IsObject()) {
				return elliptics::create_error(-EINVAL, "'periods' array must contain objects, entry at index %zd is %d",
						idx, it->GetType());
			}

			nulla::period period;
			err = parse_manifest_period(*it, period);
			if (err)
				return err;

			m_playlist->periods.emplace_back(period);
		}

		if (m_playlist->periods.empty()) {
			return elliptics::create_error(-EINVAL, "there are no periods in request");
		}

		return elliptics::error_info();
	}

	elliptics::error_info parse_manifest_period(const rapidjson::Value &mpd, nulla::period &period) {
		const auto &asets = ebucket::get_array(mpd, "asets");
		if (!asets.IsArray()) {
			return elliptics::create_error(-EINVAL, "adaptation sets 'asets' must be an array");
		}

		elliptics::error_info err;
		size_t idx = 0;
		for (auto it = asets.Begin(), it_end = asets.End(); it != it_end; ++it) {
			if (!it->IsObject()) {
				return elliptics::create_error(-EINVAL, "'asets' array must contain objects, entry at index %zd is %d",
						idx, it->GetType());
			}

			nulla::adaptation adaptation;
			err = parse_manifest_adaptation(*it, adaptation);
			if (err)
				return err;

			period.adaptations.emplace_back(adaptation);
			++idx;
		}

		if (period.adaptations.empty()) {
			return elliptics::create_error(-EINVAL, "there are no adaptations in request");
		}

		return err;
	}

	elliptics::error_info parse_manifest_adaptation(const rapidjson::Value &adaptation, nulla::adaptation &aset) {
		const auto &rsets = ebucket::get_array(adaptation, "rsets");
		if (!rsets.IsArray()) {
			return elliptics::create_error(-EINVAL, "representations 'rsets' must be an array");
		}

		elliptics::error_info err;
		size_t idx = 0;
		for (auto it = rsets.Begin(), it_end = rsets.End(); it != it_end; ++it) {
			if (!it->IsObject()) {
				return elliptics::create_error(-EINVAL, "'rsets' array must contain objects, entry at index %zd is %d",
						idx, it->GetType());
			}

			nulla::representation repr;
			err = parse_manifest_representation(*it, repr);
			if (err)
				return err;

			aset.repr_ids.push_back(repr.id);

			// this runs in @on_request() method, nothing runs in parallel,
			// we do not need to protect playlist's repr map
			m_playlist->repr.insert(std::make_pair(repr.id, repr));

			++idx;
		}

		if (aset.repr_ids.empty()) {
			return elliptics::create_error(-EINVAL, "there are no representations in request");
		}

		return err;
	}

	elliptics::error_info parse_manifest_representation(const rapidjson::Value &representation, nulla::representation &repr) {
		const auto &tracks = ebucket::get_array(representation, "tracks");
		if (!tracks.IsArray()) {
			return elliptics::create_error(-EINVAL, "'tracks' must be an array");
		}

		repr.id = std::to_string(m_playlist->meta_chunks_requested);

		const auto &init = ebucket::get_object(representation, "init");
		if (!init.IsObject()) {
			return elliptics::create_error(-EINVAL, "'init' must be an object");
		}

		const char *key = ebucket::get_string(init, "key");
		const char *bucket = ebucket::get_string(init, "bucket");
		if (!key || !bucket) {
			return elliptics::create_error(-EINVAL, "'init' must have both bucket and key: bucket: %p, key: %p",
					bucket, key);
		}

		repr.init.bucket = bucket;
		repr.init.key = key;

		elliptics::error_info err;
		size_t idx = 0;
		for (auto it = tracks.Begin(), it_end = tracks.End(); it != it_end; ++it) {
			if (!it->IsObject()) {
				return elliptics::create_error(-EINVAL, "'tracks' array must contain objects, entry at index %zd is %d",
						idx, it->GetType());
			}

			const char *key = ebucket::get_string(*it, "key");
			const char *bucket = ebucket::get_string(*it, "bucket");
			long start_msec = ebucket::get_int64(*it, "start", 0);
			long duration_msec = ebucket::get_int64(*it, "duration", 0);
			int requested_track_number = ebucket::get_int64(*it, "number", 1);

			if (!key || !bucket || duration_msec < 0 || start_msec < 0) {
				return elliptics::create_error(-EINVAL, "bucket, key, start, duration must be present, "
						"entry at index %zd is invalid: bucket: %p, key: %p, start: %ld, duration: %ld",
						idx, bucket, key, start_msec, duration_msec);
			}

			nulla::track_request tr;
			tr.key.assign(key);
			tr.bucket.assign(bucket);
			tr.start_msec = start_msec;
			tr.duration_msec = duration_msec;
			tr.requested_track_number = requested_track_number;

			repr.tracks.emplace_back(tr);

			++m_playlist->meta_chunks_requested;
			++idx;
		}

		if (repr.tracks.empty()) {
			return elliptics::create_error(-EINVAL, "there are no tracks in request");
		}

		return err;
	}

	elliptics::error_info meta_unpack(const elliptics::data_pointer &dp, nulla::track_request &tr) {
		try {
			msgpack::unpacked result;
			msgpack::unpack(&result, dp.data<char>(), dp.size());
			
			msgpack::object deserialized = result.get();
			deserialized.convert(&tr.media);

			for (auto it = tr.media.tracks.begin(), it_end = tr.media.tracks.end(); it != it_end; ++it) {
				NLOG_INFO("meta-read: %s: url: %s, bucket: %s, key: %s, requested_track_number: %d, "
						"requested_start_msec: %ld, requested_duration_msec: %ld, track: %s",
					__func__, this->request().url().to_human_readable().c_str(),
					tr.bucket.c_str(), tr.key.c_str(), tr.requested_track_number,
					tr.start_msec, tr.duration_msec,
					it->str().c_str());

				if (tr.requested_track_number == it->number) {
					long duration_msec = 0;
					if (it->duration && it->timescale)
						duration_msec = it->duration * 1000.0 / it->timescale;
					else if (it->media_duration && it->media_timescale)
						duration_msec = it->media_duration * 1000.0 / it->media_timescale;

					if (tr.start_msec >= duration_msec) {
						return elliptics::create_error(-EINVAL,
							"invalid track request start_msec: %ld, "
							"must be less than calculated media track duration_msec: %ld, track: %s",
							tr.start_msec, duration_msec, it->str().c_str());
					}

					if (!tr.duration_msec)
						tr.duration_msec = duration_msec - tr.start_msec;

					if (tr.duration_msec > duration_msec - tr.start_msec)
						tr.duration_msec = duration_msec - tr.start_msec;

					if (it->samples.size() < 2) {
						return elliptics::create_error(-EINVAL,
								"invalid track, number of sample %ld is too small, track: %s",
								it->samples.size(), it->str().c_str());
					}

					tr.requested_track_index = std::distance(tr.media.tracks.begin(), it);
				}
			}
		} catch (const std::exception &e) {
			return elliptics::create_error(-EINVAL, "meta unpack error: %s", e.what());
		}

		if (tr.requested_track_index == -1) {
			return elliptics::create_error(-ENOENT,
				"meta unpack has failed to locate requested track number %d among all received tracks",
				tr.requested_track_number);
		}

		return elliptics::error_info();
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

		// url format: http://host[:port]/prefix/playlist_id/repr/type
		// where @prefix matches thevoid handler name registered in the @Server
		// and @playlist_id is a string

		const auto &path = req.url().path_components();
		if (path.size() < 3) {
			NLOG_ERROR("url: %s: invalid path, there must be at least 3 path components: /stream/playlist_id/operation[/repr]",
					req.url().to_human_readable().c_str());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		std::string playlist_id = path[1];
		std::string operation = path[2];

		if (playlist_id.empty() || operation.empty()) {
			NLOG_ERROR("url: %s: invalid path, there must be at least 3 path components in the path "
					"/stream/playlist_id/operation[/repr] and none is allowed to be empty",
					req.url().to_human_readable().c_str());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		m_playlist = this->server()->get_playlist(playlist_id);
		if (!m_playlist) {
			NLOG_ERROR("url: %s: there is no playlist_id: %s",
					req.url().to_human_readable().c_str(), playlist_id.c_str());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		if (operation == "manifest") {
			nulla::mpd mpd(m_playlist);
			mpd.generate();

			std::string manifest = mpd.xml();

			thevoid::http_response reply;
			reply.set_code(thevoid::http_response::ok);
			reply.headers().set_content_length(manifest.size());
			reply.headers().set("Access-Control-Allow-Credentials", "true");
			reply.headers().set("Access-Control-Allow-Origin", "*");

			this->send_reply(std::move(reply), std::move(manifest));
			return;
		}

		if (path.size() < 4) {
			NLOG_ERROR("url: %s: invalid path, there must be at least 4 path components for operation %s: "
					"/stream/playlist_id/%s/repr",
					req.url().to_human_readable().c_str(), operation.c_str(), operation.c_str());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}
		std::string repr_id = path[3];
		if (repr_id.empty()) {
			NLOG_ERROR("url: %s: invalid path, there must be at least 4 path components in the path for operation %s "
					"/stream/playlist_id/%s/repr[/number] and none is allowed to be empty",
					req.url().to_human_readable().c_str(), operation.c_str(), operation.c_str());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		const auto repr_it = m_playlist->repr.find(repr_id);
		if (repr_it == m_playlist->repr.end()) {
			NLOG_ERROR("url: %s: playlist_id %s, there is no representation_id: %s",
					req.url().to_human_readable().c_str(), playlist_id.c_str(),
					repr_id.c_str());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		const nulla::representation &repr = repr_it->second;

		if (operation == "init") {
			ebucket::bucket b;
			elliptics::error_info err = this->server()->bucket_processor()->find_bucket(repr.init.bucket, b);
			if (err) {
				NLOG_ERROR("url: %s: could not find bucket %s in bucket processor: %s [%d]",
						req.url().to_human_readable().c_str(), repr.init.bucket.c_str(),
						err.message().c_str(), err.code());
				this->send_reply(thevoid::http_response::bad_request);
				return;
			}

			NLOG_INFO("%s: playlist_id: %s, init request, bucket: %s, key: %s",
					__func__, playlist_id.c_str(), repr.init.bucket.c_str(), repr.init.key.c_str());

			b->session().read_data(repr.init.key, 0, 0).connect(std::bind(&on_dash_stream_base::on_read,
				this->shared_from_this(), std::placeholders::_1, std::placeholders::_2));
			return;
		}

		if (operation != "play") {
			NLOG_ERROR("url: %s: unsupported operation %s",
					req.url().to_human_readable().c_str(), operation.c_str());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		if (path.size() != 5) {
			NLOG_ERROR("url: %s: operation %s requires 5 path components: /stream/playlist_id/%s/repr/number",
					req.url().to_human_readable().c_str(), operation.c_str(), operation.c_str());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		long number = atol(path[4].c_str());
		const auto &trf = repr.tracks.front();
		const auto &trackf = trf.track();
		long dts_start = number * m_playlist->chunk_duration_sec * trackf.media_timescale;

		const auto trit = repr.find_track_request(dts_start);
		if (trit == repr.tracks.end()) {
			NLOG_ERROR("url: %s: invalid number request: %ld, dts: %ld, must be within [%ld, %ld+samples)",
					req.url().to_human_readable().c_str(), number, dts_start,
					repr.tracks.front().dts_first_sample_offset,
					repr.tracks.back().dts_first_sample_offset);
			this->send_reply(thevoid::http_response::bad_request);
		}

		const auto &tr = *trit;

		NLOG_INFO("%s: playlist_id: %s, samples: %s/%s/%ld, playing from %s/%s", __func__,
				playlist_id.c_str(), operation.c_str(), repr_id.c_str(), number,
				tr.bucket.c_str(), tr.key.c_str());

		request_track_data(tr, number);
	}

	virtual void on_error(const boost::system::error_code &error) {
		NLOG_ERROR("buffered-write: on_error: url: %s, error: %s",
				this->request().url().to_human_readable().c_str(), error.message().c_str());
	}

	void on_read(const elliptics::sync_read_result &result, const elliptics::error_info &error)
	{
		if (error) {
			NLOG_ERROR("buffered-get: %s: url: %s: error: %s",
					__func__, this->request().url().to_human_readable().c_str(), error.message().c_str());

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

	void on_read_samples(nulla::writer_options &opt, const nulla::track_request &tr,
			const elliptics::sync_read_result &result, const elliptics::error_info &error) {
		if (error) {
			NLOG_ERROR("buffered-get: %s: url: %s: error: %s",
					__func__, this->request().url().to_human_readable().c_str(), error.message().c_str());

			auto ec = boost::system::errc::make_error_code(static_cast<boost::system::errc::errc_t>(-error.code()));
			this->close(ec);
			return;
		}

		const elliptics::read_result_entry &entry = result[0];
		const elliptics::data_pointer &sample_data = entry.file();

		opt.sample_data = sample_data.data<char>();
		opt.sample_data_size = sample_data.size();

		const nulla::track &track = tr.track();

		std::vector<char> movie_data;
		nulla::iso_writer writer(track);
		int err = writer.create(opt, movie_data);
		if (err < 0) {
			NLOG_ERROR("buffered-get: %s: url: %s: writer creation error: %d",
					__func__, this->request().url().to_human_readable().c_str(), err);

			auto ec = boost::system::errc::make_error_code(static_cast<boost::system::errc::errc_t>(err));
			this->close(ec);
			return;
		}

		thevoid::http_response reply;
		reply.set_code(swarm::http_response::ok);
		reply.headers().set_content_length(movie_data.size());
		reply.headers().set("Access-Control-Allow-Credentials", "true");
		reply.headers().set("Access-Control-Allow-Origin", "*");

		this->send_headers(std::move(reply), std::move(movie_data),
				std::bind(&on_dash_stream_base::close, this->shared_from_this(), std::placeholders::_1));
	}

	void request_track_data(const nulla::track_request &tr, long number) {
		const nulla::track &track = tr.track();

		ebucket::bucket b;
		elliptics::error_info err = this->server()->bucket_processor()->find_bucket(tr.bucket, b);
		if (err) {
			NLOG_ERROR("url: %s: could not find bucket %s in bucket processor: %s [%d]",
					this->request().url().to_human_readable().c_str(), tr.bucket.c_str(),
					err.message().c_str(), err.code());
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		number -= tr.start_number;
		u64 dtime_start = number * m_playlist->chunk_duration_sec * track.media_timescale + tr.dts_start;
		u64 time_end = (number + 1) * m_playlist->chunk_duration_sec;
		u64 dtime_end = time_end * track.media_timescale + tr.dts_start;


		ssize_t pos_start = track.sample_position_from_dts(dtime_start);
		if (pos_start < 0) {
			NLOG_ERROR("buffered-get: %s: url: %s: error: start offset is out of range, track_id: %d, track_number: %d, "
					"dtime_start: %ld, number: %ld: %zd",
					__func__, this->request().url().to_human_readable().c_str(), track.id, track.number,
					dtime_start, number, pos_start);

			auto ec = boost::system::errc::make_error_code(static_cast<boost::system::errc::errc_t>(pos_start));
			this->close(ec);
			return;
		}

		ssize_t pos_end = track.sample_position_from_dts(dtime_end);
		if (pos_end < 0) {
			pos_end = track.samples.size() - 1;
		}

		u64 start_offset = track.samples[pos_start].offset;
		u64 end_offset = track.samples[pos_end].offset + track.samples[pos_end].length;

		NLOG_INFO("buffered-get: %s: url: %s: track_id: %d, track_number: %d, samples: [%ld, %ld): "
				"media_timescale: %d, media_duration: %ld, "
				"dts_first_sample_offset: %ld, dtime: [%ld, %ld), number: %ld, data-bytes: [%ld, %ld)",
				__func__, this->request().url().to_human_readable().c_str(), track.id, track.number,
				pos_start, pos_end,
				track.media_timescale, track.media_duration,
				tr.dts_first_sample_offset, dtime_start, dtime_end, number, start_offset, end_offset);

		nulla::writer_options opt;
		opt.pos_start = pos_start;
		opt.pos_end = pos_end;
		opt.dts_start = dtime_start;
		opt.dts_end = dtime_end;
		opt.fragment_duration = 1 * track.media_timescale; // 1 second
		opt.dts_start_absolute = tr.dts_first_sample_offset - tr.dts_start;

		b->session().read_data(tr.key, start_offset, end_offset - start_offset).connect(
				std::bind(&on_dash_stream_base::on_read_samples,
					this->shared_from_this(), opt, std::cref(tr), std::placeholders::_1, std::placeholders::_2));
	}


private:
	nulla::playlist_t m_playlist;
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
		srand(time(NULL));

		if (!elliptics_init(config))
			return false;

		on<on_dash_manifest<nulla_server>>(
			options::prefix_match("/dash_manifest"),
			options::methods("POST")
		);

		on<on_dash_stream<nulla_server>>(
			options::prefix_match("/stream"),
			options::methods("GET")
		);

		return true;
	}

	std::shared_ptr<ebucket::bucket_processor> bucket_processor() const {
		return m_bp;
	}

	std::string store_playlist(nulla::playlist_t &playlist) {
		elliptics::key k(std::to_string(m_playlist_seq++) + "." + std::to_string(rand()));
		k.transform(*m_session);

		char dst[2*DNET_ID_SIZE+1];
		playlist->cookie.assign(dnet_dump_id_len_raw(k.raw_id().id, DNET_ID_SIZE, dst));

		std::lock_guard<std::mutex> guard(m_playlists_lock);
		m_playlists.insert(std::make_pair(playlist->cookie, playlist));
		return playlist->cookie;
	}

	nulla::playlist_t get_playlist(const std::string &cookie) {
		std::lock_guard<std::mutex> guard(m_playlists_lock);
		auto it = m_playlists.find(cookie);
		if (it != m_playlists.end()) {
			return it->second;
		}

		return nulla::playlist_t();
	}

private:
	std::shared_ptr<elliptics::node> m_node;
	std::unique_ptr<elliptics::session> m_session;
	std::shared_ptr<ebucket::bucket_processor> m_bp;

	std::mutex m_playlists_lock;
	std::atomic_long m_playlist_seq;
	std::map<std::string, nulla::playlist_t> m_playlists;

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
			m_session.reset(new elliptics::session(node));

			if (!m_session->get_routes().size()) {
				NLOG_ERROR("Didn't add any remote node, exiting.");
				return false;
			}
		} catch (const std::exception &e) {
			NLOG_ERROR("Could not add any out of %ld nodes.", remotes.size());
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

