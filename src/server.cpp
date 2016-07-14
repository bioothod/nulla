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
#include "nulla/dash_playlist.hpp"
#include "nulla/expiration.hpp"
#include "nulla/jsonvalue.hpp"
#include "nulla/hls_playlist.hpp"
#include "nulla/iso_reader.hpp"
#include "nulla/iso_writer.hpp"
#include "nulla/log.hpp"
#include "nulla/mpeg2ts_writer.hpp"
#include "nulla/playlist.hpp"
#include "nulla/upload.hpp"
#include "nulla/utils.hpp"

#include <ebucket/bucket_processor.hpp>

#include <elliptics/session.hpp>

#include <thevoid/rapidjson/stringbuffer.h>
#include <thevoid/rapidjson/prettywriter.h>
#include <thevoid/rapidjson/document.h>

#include <thevoid/server.hpp>
#include <thevoid/stream.hpp>

#include <unistd.h>
#include <signal.h>

#include <atomic>

using namespace ioremap;

template <typename Server, typename Stream>
class on_dash_manifest_base : public thevoid::simple_request_stream<Server>, public std::enable_shared_from_this<Stream> {
public:
	virtual void on_request(const thevoid::http_request &req, const boost::asio::const_buffer &buffer) {
		m_playlist = std::shared_ptr<nulla::raw_playlist>(new nulla::raw_playlist());

		m_xreq = req.request_id();
		m_trace = req.trace_bit();

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

		for (auto &repr_pair: m_playlist->repr) {
			err = request_track_info(repr_pair.second);
			if (err) {
				NLOG_ERROR("url: %s: repr: %s, could not request track info, error: %s [%d]",
					req.url().to_human_readable().c_str(),
					repr_pair.first.c_str(), err.message().c_str(), err.code());
				this->send_reply(thevoid::http_response::bad_request);
				return;
			}
		}
	}

	virtual void on_error(const boost::system::error_code &error) {
		NLOG_ERROR("buffered-read: on_error: url: %s, error: %s",
			this->request().url().to_human_readable().c_str(), error.message().c_str());
	}

private:
	nulla::playlist_t m_playlist;
	uint64_t m_xreq = 0;
	int m_trace = 0;

	elliptics::error_info update_periods(nulla::representation &repr) {
		repr.duration_msec = 0;
		long dts_first_sample_offset = 0;
		long number = 0;

		for (auto &tr : repr.tracks) {
			nulla::track &track = tr.media.tracks[tr.requested_track_index];

			tr.dts_start = tr.start_msec * track.media_timescale / 1000;
			ssize_t start_pos = track.sample_position_from_dts(tr.dts_start, true);
			if (start_pos < 0) {
				return elliptics::create_error(-EINVAL,
						"could not locate sample for dts_start: %ld, track: %s",
						tr.dts_start, track.str().c_str());
			}
			tr.dts_start = track.samples[start_pos].dts;

			long dts_end = tr.dts_start + tr.duration_msec * track.media_timescale / 1000;
			ssize_t end_pos = track.sample_position_from_dts(dts_end, false);
			if (end_pos < 0)
				end_pos = track.samples.size() - 1;

			memmove((char *)track.samples.data(), track.samples.data() + start_pos,
					sizeof(nulla::sample) * (end_pos - start_pos + 1));
			track.samples.resize(end_pos - start_pos + 1);

			for (auto &s : track.samples) {
				s.dts -= tr.dts_start;
			}


			tr.dts_first_sample_offset = dts_first_sample_offset;
			tr.start_number = number;

			repr.duration_msec += tr.duration_msec;
			number += (tr.duration_msec + 1000 * m_playlist->chunk_duration_sec - 1) /
				(1000 * m_playlist->chunk_duration_sec);

			const nulla::sample &end_sample = track.samples[track.samples.size() - 1];
			const nulla::sample &prev_sample = track.samples[track.samples.size() - 2];
			long last_diff_dts = end_sample.dts - prev_sample.dts;

			dts_first_sample_offset += end_sample.dts + last_diff_dts;

			NLOG_INFO("track: %s, dts: [%lu, %lu), start_number: %ld, dts_first_sample_offset: %lu\n",
					track.str().c_str(), track.samples[0].dts, end_sample.dts + last_diff_dts,
					tr.start_number, tr.dts_first_sample_offset);
		}

		// set period duration to the smallest representation duration
		if (m_playlist->duration_msec == 0 || m_playlist->duration_msec > repr.duration_msec)
			m_playlist->duration_msec = repr.duration_msec;

		return elliptics::error_info();
	}

	void truncate_duration(nulla::representation &repr) {
		// period duration has been set to the smallest representation duration,
		// we have to update all adaptations to be the same lenght
		long duration_msec = m_playlist->duration_msec;
		repr.duration_msec = duration_msec;

		for (auto &tr : repr.tracks) {
			if (duration_msec < 0) {
				tr.duration_msec = 0;
				continue;
			}

			duration_msec -= tr.duration_msec;
			if (duration_msec < 0) {
				tr.duration_msec += duration_msec;
			}
		}
	}

	elliptics::error_info check_and_send_manifest() {
		elliptics::error_info err;
		if (m_playlist->meta_chunks_read != m_playlist->meta_chunks_requested)
			return err;

		for (auto &repr_pair: m_playlist->repr) {
			err = update_periods(repr_pair.second);
			if (err)
				return err;
		}

		for (auto &repr_pair: m_playlist->repr) {
			truncate_duration(repr_pair.second);
		}

		m_playlist->id = this->server()->store_playlist(m_playlist);
		m_playlist->base_url = this->server()->hostname() + "/stream/" + m_playlist->id + "/";

		send_manifest();
		return err;
	}

	void send_manifest() {
		nulla::JsonValue ret;

		rapidjson::Value pval(m_playlist->id.c_str(), m_playlist->id.size(), ret.GetAllocator());
		ret.AddMember("id", pval, ret.GetAllocator());

		rapidjson::Value pbase(m_playlist->base_url.c_str(), m_playlist->base_url.size(), ret.GetAllocator());
		ret.AddMember("base_url", pbase, ret.GetAllocator());

		std::string playlist_url = m_playlist->base_url + "playlist";
		rapidjson::Value purl(playlist_url.c_str(), playlist_url.size(), ret.GetAllocator());
		ret.AddMember("playlist_url", purl, ret.GetAllocator());

		std::string data = ret.ToString();

		thevoid::http_response reply;
		reply.set_code(swarm::http_response::ok);
		reply.headers().set_content_length(data.size());
		reply.headers().set("Access-Control-Allow-Origin", "*");

		this->send_reply(std::move(reply), std::move(data));
	}

	void on_read_meta(const std::string &repr_id, size_t track_position,
			const elliptics::sync_read_result &result, const elliptics::error_info &error) {
		if (error) {
			NLOG_ERROR("meta-read: repr: %s, track_position: %ld, error: %s [%d]",
					repr_id.c_str(), track_position, error.message().c_str(), error.code());

			if (error.code() == -ENOENT) {
				this->send_reply(thevoid::http_response::not_found);
			} else {
				this->send_reply(thevoid::http_response::service_unavailable);
			}
			return;
		}

		// this callback can be invoked in parallel, but it does not change playlist's repr map,
		// two callbacks can update the same representation in parallel, but they update
		// different track requests and do not change mutual track alignment (like positions in std::vector or vector itself),
		// so we do not need to protect it
		const auto repr_it = m_playlist->repr.find(repr_id);
		if (repr_it == m_playlist->repr.end()) {
			NLOG_ERROR("meta-read: repr: %s, track_position: %ld, could not find representation",
					repr_id.c_str(), track_position);
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		nulla::representation &repr = repr_it->second;

		// this is just a sanity check
		if (track_position >= repr.tracks.size()) {
			NLOG_ERROR("meta-read: repr: %s, track_position: %zd, tracks_size: %zd: position is out of band",
					repr_id.c_str(), track_position, repr_it->second.tracks.size());

			this->send_reply(thevoid::http_response::service_unavailable);
			return;
		}

		nulla::track_request &tr = repr.tracks[track_position];

		const elliptics::read_result_entry &entry = result[0];
		elliptics::error_info err = meta_unpack(entry.file(), tr);
		if (err) {
			NLOG_ERROR("meta-read: repr: %s, track_position: %zd, "
				"track: bucket: %s, key: %s, could not unpack metadata: %s [%d]",
					repr_id.c_str(), track_position, tr.bucket.c_str(), tr.key.c_str(),
					err.message().c_str(), err.code());

			if (err.code() == -ENOENT) {
				this->send_reply(thevoid::http_response::not_found);
				return;
			}

			this->send_reply(thevoid::http_response::service_unavailable);
			return;
		}

		NLOG_INFO("meta-read: repr: %s, track_position: %zd: track: bucket: %s, key: %s, media-tracks: %zd",
			repr_id.c_str(), track_position, tr.bucket.c_str(), tr.key.c_str(), tr.media.tracks.size());

		++m_playlist->meta_chunks_read;
		err = check_and_send_manifest();
		if (err) {
			NLOG_ERROR("meta-read: repr: %s, track_position: %zd, could not create and send manifest: %s [%d]",
				repr_id.c_str(), track_position, err.message().c_str(), err.code());
			this->send_reply(thevoid::http_response::service_unavailable);
			return;
		}
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

		m_playlist->type = ebucket::get_string(doc, "type", "dash");

		m_playlist->expires_at = std::chrono::system_clock::now() +
			std::chrono::seconds(ebucket::get_int64(doc, "timeout_sec", 10));
		m_playlist->chunk_duration_sec = ebucket::get_int64(doc, "chunk_duration_sec", 5);

		elliptics::error_info err = elliptics::create_error(-EINVAL, "there are no audio/video tracks in request");

		const auto &audio = ebucket::get_object(doc, "audio");
		if (audio.IsObject()) {
			nulla::representation repr;
			err = parse_manifest_tracks(audio, repr);
			if (err) {
				return elliptics::create_error(err.code(), "could not parse audio tracks: %s", err.message().c_str());
			}

			repr.id = "audio";
			if (repr.tracks.size() != 0)
				m_playlist->repr.insert(std::make_pair(repr.id, repr));
		}
		const auto &video = ebucket::get_object(doc, "video");
		if (video.IsObject()) {
			nulla::representation repr;
			err = parse_manifest_tracks(video, repr);
			if (err) {
				return elliptics::create_error(err.code(), "could not parse video tracks: %s", err.message().c_str());
			}

			repr.id = "video";
			if (repr.tracks.size() != 0)
				m_playlist->repr.insert(std::make_pair(repr.id, repr));
		}

		return err;
	}

	elliptics::error_info parse_manifest_tracks(const rapidjson::Value &representation, nulla::representation &repr) {
		bool skip = ebucket::get_bool(representation, "skip", false);
		if (skip) {
			return elliptics::error_info();
		}

		const auto &tracks = ebucket::get_array(representation, "tracks");
		if (!tracks.IsArray()) {
			return elliptics::create_error(-EINVAL, "'tracks' must be an array");
		}

		repr.id = std::to_string(m_playlist->meta_chunks_requested);

		elliptics::error_info err;
		size_t idx = 0;
		for (auto it = tracks.Begin(), it_end = tracks.End(); it != it_end; ++it) {
			if (!it->IsObject()) {
				return elliptics::create_error(-EINVAL, "'tracks' array must contain objects, entry at index %zd is %d",
						idx, it->GetType());
			}

			bool skip = ebucket::get_bool(*it, "skip", false);
			if (skip) {
				continue;
			}

			const char *key = ebucket::get_string(*it, "key");
			const char *meta_key = ebucket::get_string(*it, "meta_key");
			const char *bucket = ebucket::get_string(*it, "bucket");
			long start_msec = ebucket::get_int64(*it, "start", 0);
			long duration_msec = ebucket::get_int64(*it, "duration", 0);
			int requested_track_number = ebucket::get_int64(*it, "number", 1);

			if (!key || !meta_key || !bucket || duration_msec < 0 || start_msec < 0) {
				return elliptics::create_error(-EINVAL, "bucket, key, meta_key, start, duration must be present, "
						"entry at index %zd is invalid: bucket: %p, key: %p, start: %ld, duration: %ld",
						idx, bucket, key, start_msec, duration_msec);
			}

			nulla::track_request tr;
			tr.key.assign(key);
			tr.meta_key.assign(meta_key);
			tr.bucket.assign(bucket);
			tr.start_msec = start_msec;
			tr.duration_msec = duration_msec;
			tr.requested_track_number = requested_track_number;

			repr.tracks.emplace_back(tr);

			++m_playlist->meta_chunks_requested;
			++idx;
		}

		return err;
	}

	elliptics::error_info request_track_info(const nulla::representation &repr) {
		// we have to run this loop after the whole @repr.tracks array has been filled,
		// since completion callback will modify its entries, and there are no locks
		int idx = 0;
		elliptics::error_info err;

		for (auto &tr: repr.tracks) {
			ebucket::bucket b;
			err = this->server()->bucket_processor()->find_bucket(tr.bucket, b);
			if (err) {
				return elliptics::create_error(err.code(),
						"idx: %d, could not find bucket %s in bucket processor: %s [%d]",
						idx, tr.bucket.c_str(), err.message().c_str(), err.code());
			}

			auto session = b->session();
			session.set_filter(elliptics::filters::positive);
			session.set_trace_id(m_xreq);
			session.set_trace_bit(m_trace);

			session.read_data(tr.meta_key, 0, 0).connect(
				std::bind(&on_dash_manifest_base::on_read_meta,
					this->shared_from_this(), repr.id, idx,
					std::placeholders::_1, std::placeholders::_2));

			++idx;
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

					if (tr.duration_msec < m_playlist->chunk_duration_sec * 1000) {
						m_playlist->chunk_duration_sec = tr.duration_msec / 1000;
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

		m_xreq = req.request_id();
		m_trace = req.trace_bit();

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

		if (operation == "playlist") {
			if (std::chrono::system_clock::now() > m_playlist->expires_at) {
				NLOG_ERROR("url: %s: has already expired", req.url().to_human_readable().c_str());
				this->send_reply(thevoid::http_response::request_timeout);
				return;
			}

			std::string pl;
			thevoid::http_response reply;
			reply.set_code(thevoid::http_response::ok);
			reply.headers().set("Access-Control-Allow-Origin", "*");

			if (m_playlist->type == "dash") {
				nulla::mpd mpd(m_playlist);
				mpd.generate();

				pl = mpd.xml();
				reply.headers().set_content_type("application/dash+xml");
			} else if (m_playlist->type == "hls") {
				nulla::m3u8 m(m_playlist);
				m.generate();

				if (path.size() == 3) {
					pl = m.main_playlist();
				} else {
					std::string variant = path[3];
					pl = m.variant_playlist(variant);
				}
			}

			if (pl.empty()) {
				NLOG_ERROR("url: %s: empty playlist for type %s",
						req.url().to_human_readable().c_str(), m_playlist->type.c_str());
				this->send_reply(thevoid::http_response::bad_request);
				return;
			}


			reply.headers().set_content_length(pl.size());

			this->send_reply(std::move(reply), std::move(pl));
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
			if (std::chrono::system_clock::now() > m_playlist->expires_at) {
				NLOG_ERROR("url: %s: has already expired", req.url().to_human_readable().c_str());
				this->send_reply(thevoid::http_response::request_timeout);
				return;
			}

			const auto &tr = repr.tracks.front();

			NLOG_INFO("url: %s: playlist_id: %s, init request", req.url().to_human_readable().c_str(), playlist_id.c_str());

			nulla::writer_options opt;
			memset(&opt, 0, sizeof(opt));

			const nulla::track &track = tr.track();

			std::vector<char> init_data, movie_data;
			nulla::iso_writer writer(this->server()->tmp_dir(), track);
			int err = writer.create(opt, true, init_data, movie_data);
			if (err < 0) {
				NLOG_ERROR("url: %s: playlist_id: %s: writer creation error: %d",
						__func__, this->request().url().to_human_readable().c_str(), err);

				this->send_reply(thevoid::http_response::internal_server_error);
				return;
			}

			thevoid::http_response reply;
			reply.set_code(swarm::http_response::ok);
			reply.headers().set_content_length(init_data.size());
			reply.headers().set("Access-Control-Allow-Origin", "*");

			this->send_reply(std::move(reply), std::move(init_data));
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

		if (std::chrono::system_clock::now() > m_playlist->expires_at +
				std::chrono::seconds(number * m_playlist->chunk_duration_sec)) {
			NLOG_ERROR("url: %s: has already expired", req.url().to_human_readable().c_str());
			this->send_reply(thevoid::http_response::request_timeout);
			return;
		}

		const auto trit = repr.find_track_request(number);
		if (trit == repr.tracks.end()) {
			const auto &trf = repr.tracks.front();
			NLOG_ERROR("url: %s: invalid number request: %ld, dts: %ld, must be within [%ld, %ld+samples)",
					req.url().to_human_readable().c_str(),
					number, number * m_playlist->chunk_duration_sec * trf.track().media_timescale,
					repr.tracks.front().dts_first_sample_offset,
					repr.tracks.back().dts_first_sample_offset);
			this->send_reply(thevoid::http_response::bad_request);
			return;
		}

		const auto &tr = *trit;

		NLOG_INFO("%s: playlist_id: %s, samples: %s/%s/%ld, playing from %s/%s", __func__,
				playlist_id.c_str(), operation.c_str(), repr_id.c_str(), number,
				tr.bucket.c_str(), tr.key.c_str());

		request_track_data(tr, number);
	}

	void on_read_samples(nulla::writer_options &opt, const nulla::track_request &tr,
			const elliptics::sync_read_result &result, const elliptics::error_info &error) {
		if (error) {
			NLOG_ERROR("buffered-get: %s: url: %s: error: %s",
					__func__, this->request().url().to_human_readable().c_str(), error.message().c_str());

			this->send_reply(thevoid::http_response::internal_server_error);
			return;
		}

		const elliptics::read_result_entry &entry = result[0];
		const elliptics::data_pointer &sample_data = entry.file();

		opt.sample_data = sample_data.data<char>();
		opt.sample_data_size = sample_data.size();

		const nulla::track &track = tr.track();

		std::vector<char> movie_data;
		int err;
		if (m_playlist->type == "dash") {
			std::vector<char> init_data;
			nulla::iso_writer writer(this->server()->tmp_dir(), track);
			err = writer.create(opt, false, init_data, movie_data);
		} else {
			nulla::mpeg2ts_writer writer(this->server()->tmp_dir(), track);
			err = writer.create(opt, movie_data);
		}

		if (err < 0) {
			NLOG_ERROR("buffered-get: %s: url: %s: writer creation error: %d",
					__func__, this->request().url().to_human_readable().c_str(), err);

			this->send_reply(thevoid::http_response::internal_server_error);
			return;
		}

		thevoid::http_response reply;
		reply.set_code(swarm::http_response::ok);
		reply.headers().set_content_length(movie_data.size());
		reply.headers().set("Access-Control-Allow-Origin", "*");

		this->send_reply(std::move(reply), std::move(movie_data));
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
		u64 dtime_start = number * m_playlist->chunk_duration_sec * track.media_timescale;
		u64 dtime_end = (number + 1) * m_playlist->chunk_duration_sec * track.media_timescale;


		ssize_t pos_start = track.sample_position_from_dts(dtime_start, true);
		if (pos_start < 0) {
			NLOG_ERROR("buffered-get: %s: url: %s: error: start offset is out of range, track_id: %d, track_number: %d, "
					"dtime_start: %ld, number: %ld: %zd",
					__func__, this->request().url().to_human_readable().c_str(), track.id, track.number,
					dtime_start, number, pos_start);

			this->send_reply(thevoid::http_response::internal_server_error);
			return;
		}

		ssize_t pos_end = track.sample_position_from_dts(dtime_end, false);
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
		opt.dts_start_absolute = tr.dts_first_sample_offset;

		auto session = b->session();
		session.set_filter(elliptics::filters::positive);
		session.set_trace_id(m_xreq);
		session.set_trace_bit(m_trace);
		session.read_data(tr.key, start_offset, end_offset - start_offset).connect(
				std::bind(&on_dash_stream_base::on_read_samples,
					this->shared_from_this(), opt, std::cref(tr), std::placeholders::_1, std::placeholders::_2));
	}


private:
	nulla::playlist_t m_playlist;
	uint64_t m_xreq = 0;
	int m_trace = 0;
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
			options::prefix_match("/manifest"),
			options::methods("POST")
		);

		on<on_dash_stream<nulla_server>>(
			options::prefix_match("/stream"),
			options::methods("GET")
		);

		on<nulla::on_upload<nulla_server>>(
			options::prefix_match("/upload"),
			options::methods("POST", "PUT")
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
		playlist->id.assign(dnet_dump_id_len_raw(k.raw_id().id, DNET_ID_SIZE, dst));

		auto expires_at = playlist->expires_at + std::chrono::milliseconds(playlist->duration_msec);

		{
			std::lock_guard<std::mutex> guard(m_playlists_lock);
			m_playlists.insert(std::make_pair(playlist->id, playlist));
		}

		m_expiration.insert(expires_at, std::bind(&nulla_server::remove_playlist, this, playlist->id));
		return playlist->id;
	}

	nulla::playlist_t get_playlist(const std::string &id) {
		std::lock_guard<std::mutex> guard(m_playlists_lock);
		auto it = m_playlists.find(id);
		if (it != m_playlists.end()) {
			return it->second;
		}

		return nulla::playlist_t();
	}

	const std::string &tmp_dir() const {
		return m_tmp_dir;
	}

	const std::string &hostname() const {
		return m_hostname;
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

	std::string m_tmp_dir;
	std::string m_hostname;

	nulla::expiration m_expiration;

	void remove_playlist(const std::string &cookie) {
		std::lock_guard<std::mutex> guard(m_playlists_lock);
		m_playlists.erase(cookie);
	}

	bool elliptics_init(const rapidjson::Value &config) {
		dnet_config node_config;
		memset(&node_config, 0, sizeof(node_config));

		if (!prepare_config(config, node_config)) {
			return false;
		}

		if (!prepare_server(config)) {
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

		if (!config.HasMember("metadata_groups")) {
			NLOG_ERROR("\"application.metadata_groups\" field is missed");
			return false;
		}

		std::vector<int> mgroups;
		auto &groups_meta_array = config["metadata_groups"];
		for (auto it = groups_meta_array.Begin(), end = groups_meta_array.End(); it != end; ++it) {
			if (it->IsInt())
				mgroups.push_back(it->GetInt());
		}

		if (!m_bp->init(mgroups, std::vector<std::string>(bnames.begin(), bnames.end())))
			return false;

		return true;
	}

	bool prepare_server(const rapidjson::Value &config) {
		const char *tmp_dir = ebucket::get_string(config, "tmp_dir", "/tmp/");
		m_tmp_dir.assign(tmp_dir);

		const char *hostname = ebucket::get_string(config, "hostname");
		if (!hostname) {
			NLOG_ERROR("\"hostname\" field is missing, it will be used in MPD to generate base URL");
			return false;
		}

		m_hostname.assign(hostname);

		av_register_all();
		//av_log_set_level(AV_LOG_VERBOSE);

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

