#ifndef __NULLA_ISO_READER_HPP
#define __NULLA_ISO_READER_HPP

#include "nulla/sample.hpp"

#include <gpac/constants.h>
#include <gpac/media_tools.h>
#include <gpac/tools.h>
#include <gpac/isomedia.h>

#include <gpac/internal/isomedia_dev.h>

#include <msgpack.h>
#include <msgpack.hpp>

#include <iostream>
#include <sstream>

namespace ioremap { namespace nulla {

struct media {
	enum {
		serialization_version_2 = 2
	};

	std::vector<track>		tracks;

	template <typename Stream>
	void msgpack_pack(msgpack::packer<Stream> &o) const {
		o.pack_array(serialization_version_2);
		o.pack((int)serialization_version_2);
		o.pack(tracks);
	}
	void msgpack_unpack(msgpack::object o) {
		if (o.type != msgpack::type::ARRAY) {
			std::ostringstream ss;
			ss << "page unpack: type: " << o.type <<
				", must be: " << msgpack::type::ARRAY <<
				", size: " << o.via.array.size;
			throw std::runtime_error(ss.str());
		}

		msgpack::object *p = o.via.array.ptr;
		const uint32_t size = o.via.array.size;
		uint16_t version = 0;
		p[0].convert(&version);
		switch (version) {
		case ioremap::nulla::media::serialization_version_2: {
			if (size != ioremap::nulla::media::serialization_version_2) {
				std::ostringstream ss;
				ss << "page unpack: array size mismatch: read: " << size <<
					", must be: " << ioremap::nulla::media::serialization_version_2;
				throw std::runtime_error(ss.str());
			}

			p[1].convert(&tracks);
			break;
		}
		default: {
			std::ostringstream ss;
			ss << "page unpack: version mismatch: read: " << version <<
				", there is no such packing version ";
			throw std::runtime_error(ss.str());
		}
		}
	}
};

class iso_reader {
public:
	iso_reader() {}

	iso_reader(const char *filename) {
		gf_sys_init(GF_FALSE);
		gf_log_set_tool_level(GF_LOG_ALL, GF_LOG_WARNING);

		GF_Err e;
		u64 missing_bytes = 0;

		e = gf_isom_open_progressive(filename, 0, 0, &m_movie, &missing_bytes);
		if (m_movie == NULL || (e != GF_OK && e != GF_ISOM_INCOMPLETE_FILE)) {
			std::ostringstream ss;
			ss << "could not open file " << filename << " for reading: " << gf_error_to_string(e);
			throw std::runtime_error(ss.str());
		}
	}
	virtual ~iso_reader() {
		gf_isom_close(m_movie);
		gf_sys_close();
	}

	int parse_meta() {
		GF_Err e = GF_BUFFER_TOO_SMALL;

		// tracks start from 1
		for (u32 i = 1; i <= gf_isom_get_track_count(m_movie); i++) {
			nulla::track t;

			t.number = i;
			parse_track_metadata(t);

			m_media.tracks.emplace_back(t);
		}

		return (int)e;
	}

	int parse_tracks() {
		GF_Err e = GF_BUFFER_TOO_SMALL;

		for (auto &t: m_media.tracks) {
			e = parse_track_metadata(t);
			if (e != GF_OK) {
				return (int)e;
			}

			e = parse_track_samples(t);
			if (e != GF_OK) {
				return (int)e;
			}
		}

		return 0;
	}

	int parse() {
		int err = parse_meta();
		if (err)
			return err;

		return parse_tracks();
	}

	std::string pack() const {
		std::stringstream buffer;
		msgpack::pack(buffer, m_media);
		buffer.seekg(0);

		return buffer.str();
	}

	const media &get_media() const {
		return m_media;
	}

protected:
	GF_ISOFile *m_movie;
	media m_media;

	GF_Err parse_track_metadata(nulla::track &t) {
		GF_Err e;

		u64 media_duration = gf_isom_get_media_duration(m_movie, t.number);
		u64 tduration = gf_isom_get_track_duration(m_movie, t.number);

		if (media_duration == 0)
			return GF_OK;

		t.id = gf_isom_get_track_id(m_movie, t.number);

		t.media_type = gf_isom_get_media_type(m_movie, t.number);
		t.media_subtype = gf_isom_get_media_subtype(m_movie, t.number, 1);
		t.media_subtype_mpeg4 = gf_isom_get_mpeg4_subtype(m_movie, t.number, 1);
		t.media_timescale = gf_isom_get_media_timescale(m_movie, t.number);
		t.media_duration = media_duration;

		t.timescale = gf_isom_get_timescale(m_movie);
		t.duration = tduration;
		t.data_size = gf_isom_get_media_data_size(m_movie, t.number);

		float duration;
		if (t.duration && t.timescale) {
			duration = (float)t.duration / (float)t.timescale;
		} else if (t.media_duration && t.media_timescale) {
			duration = (float)t.media_duration / (float)t.media_timescale;
		} else {
			duration = 1;
		}
		t.bandwidth = t.data_size * 8 / duration;

		switch (t.media_type) {
		case GF_ISOM_MEDIA_AUDIO:
			t.mime_type = "audio/mp4";
			gf_isom_get_audio_info(m_movie, t.number, 1, &t.audio.sample_rate, &t.audio.channels, &t.audio.bps);

			break;
		case GF_ISOM_MEDIA_VISUAL:
			t.mime_type = "video/mp4";

			gf_isom_get_visual_info(m_movie, t.number, 1, &t.video.width, &t.video.height);
			t.video.fps_num = gf_isom_get_media_timescale(m_movie, t.number);
			t.video.fps_denum = gf_isom_get_sample_duration(m_movie, t.number, 2);

			gf_isom_get_pixel_aspect_ratio(m_movie, t.number, 1, &t.video.sar_w, &t.video.sar_h);

			break;
		default:
			t.mime_type = "weird/mp4";
			break;
		}

		GF_ESD *esd = gf_isom_get_esd(m_movie, t.number, 1);
		if (esd) {
			t.esd = nulla::esd(esd);
		}
		gf_odf_desc_del((GF_Descriptor *)esd);

		char codec[128];
		e = gf_media_get_rfc_6381_codec_name(m_movie, t.number, codec, GF_FALSE, GF_FALSE);
		if (e != GF_OK) {
			return e;
		}

		t.codec.assign(codec);
		return GF_OK;
	}

	GF_Err parse_track_samples(track &t) {
		GF_ISOSample *iso_sample;
		u32 sample_count;
		u32 di; /*descriptor index*/
		GF_Err e;
		u64 offset;

		sample_count = gf_isom_get_sample_count(m_movie, t.number);
		if (sample_count == 0) {
			return GF_OK;
		}

		for (u32 sidx = 1; sidx < sample_count + 1; ++sidx) {
			iso_sample = gf_isom_get_sample_info(m_movie, t.number, sidx, &di, &offset);
			if (!iso_sample) {
				e = gf_isom_last_error(m_movie);
				fprintf(stdout, "Could not get sample, track_number: %d, sample_index: %d, di: %d, error: %s\n",
						t.number, sidx, di, gf_error_to_string(e));
				return e;
			}

			sample sam;
			sam.dts = iso_sample->DTS;
			sam.cts_offset = iso_sample->CTS_Offset;
			sam.offset = offset;
			sam.length = iso_sample->dataLength;
			sam.is_rap = iso_sample->IsRAP;
			sam.di = di;

			t.samples.emplace_back(sam);

			/* if you want the sample description data, you can call:
			   GF_Descriptor *desc = gf_isom_get_decoder_config(m_movie, reader->track_handle, di);
			*/

#if 0
			fprintf(stdout, "Found sample #%5d (#%5d) of length %8d, RAP: %d, DTS: %ld, CTS: %ld, data-offset: %ld\n",
					sidx, sample_count,
					iso_sample->dataLength, iso_sample->IsRAP,
					(unsigned long)iso_sample->DTS,
					(unsigned long)iso_sample->DTS+iso_sample->CTS_Offset,
					(unsigned long)offset);
#endif
			/*release the sample data, once you're done with it*/
			gf_isom_sample_del(&iso_sample);
		}

		return GF_OK;
	}
};

class iso_memory_reader : public iso_reader
{
public:
	iso_memory_reader(const char *ptr, size_t size) {
		gf_sys_init(GF_FALSE);
		gf_log_set_tool_level(GF_LOG_ALL, GF_LOG_WARNING);

		GF_Err e;
		u64 missing_bytes = 0;

		m_cached.insert(m_cached.end(), ptr, ptr + size);

		char buf[128];
		snprintf(buf, sizeof(buf), "gmem://%ld@%p", m_cached.size(), m_cached.data());

		e = gf_isom_open_progressive(buf, 0, 0, &m_movie, &missing_bytes);
		if (m_movie == NULL || (e != GF_OK && e != GF_ISOM_INCOMPLETE_FILE)) {
			std::ostringstream ss;
			ss << "could not open region of memory"
				", buffer: " << buf <<
				", error: " << gf_error_to_string(e) << ", code: " << e <<
				", missing_bytes: " << missing_bytes;
			throw std::runtime_error(ss.str());
		}

		int err = parse_meta();
		if (err) {
			std::ostringstream ss;
			ss << "could not parse metadata, buffer: " << buf << ", error: " << gf_error_to_string((GF_Err)e);
			throw std::runtime_error(ss.str());
		}

		if (m_media.tracks.empty()) {
			std::ostringstream ss;
			ss << "invalid buffer (probably too small), could not find track metadata, buffer: " << buf;
			throw std::runtime_error(ss.str());
		}

		err = parse_tracks();
		if (err) {
			std::ostringstream ss;
			ss << "could not parse tracks, buffer: " << buf << ", error: " << gf_error_to_string((GF_Err)e);
			throw std::runtime_error(ss.str());
		}

		err = cleanup();
		if (err) {
			std::ostringstream ss;
			ss << "could not cleanup stream, buffer: " << buf << ", error: " << gf_error_to_string((GF_Err)e);
			throw std::runtime_error(ss.str());
		}
	}

	void feed(const char *ptr, size_t size) {
		GF_Err e;
		u64 missing_bytes;

		m_cached.insert(m_cached.end(), ptr, ptr + size);

		char buf[128];
		snprintf(buf, sizeof(buf), "gmem://%ld@%p", m_cached.size(), m_cached.data());

		e = gf_isom_refresh_fragmented(m_movie, &missing_bytes, buf);
		if (e != GF_OK && e != GF_ISOM_INCOMPLETE_FILE) {
			std::ostringstream ss;
			ss << "could not refresh stream: " << buf <<
				": missing bytes: " << missing_bytes <<
				", error: " << gf_error_to_string(e);
			throw std::runtime_error(ss.str());
		}

		int err = parse_tracks();
		if (err) {
			std::ostringstream ss;
			ss << "could not parse tracks: " << buf <<
				": missing bytes: " << missing_bytes <<
				", error: " << gf_error_to_string((GF_Err)e);
			throw std::runtime_error(ss.str());
		}

		err = cleanup();
		if (err != GF_OK) {
			std::ostringstream ss;
			ss << "could not cleanup stream: " << buf <<
				", error: " << gf_error_to_string((GF_Err)e);
			throw std::runtime_error(ss.str());
		}
	}

	int cleanup() {
		u64 new_buffer_start = 0;
		u64 missing_bytes;

		/* release internal structures associated with the samples read so far */
		gf_isom_reset_tables(m_movie, GF_TRUE);

		/* release the associated input data as well */
		GF_Err e = gf_isom_reset_data_offset(m_movie, &new_buffer_start);
		if (e != GF_OK) {
			fprintf(stdout, "could not reset data offset: data_size: %ld, new_buffer_start: %ld, error: %s [%d]\n",
				m_cached.size(), new_buffer_start, gf_error_to_string(e), e);
			return (int)e;
		}
#if 0
		fprintf(stdout, "cleanup: resize: data offset: data_size: %ld, new_buffer_start: %ld, error: %s [%d]\n",
				m_cached.size(), new_buffer_start, gf_error_to_string(e), e);
#endif
		if (new_buffer_start < m_cached.size()) {
			memmove((char *)m_cached.data(), (char *)m_cached.data() + new_buffer_start, m_cached.size() - new_buffer_start);
			m_cached.resize(m_cached.size() - new_buffer_start);
		}

		char buf[128];
		snprintf(buf, sizeof(buf), "gmem://%ld@%p", m_cached.size(), m_cached.data());
		e = gf_isom_refresh_fragmented(m_movie, &missing_bytes, buf);
		if (e != GF_OK && e != GF_ISOM_INCOMPLETE_FILE) {
			fprintf(stdout, "cleanup: resize: could not refresh fragmented mp4, buf: %s, missing_bytes: %ld, error: %s [%d]\n",
					buf, missing_bytes, gf_error_to_string(e), e);
			return (int)e;
		}

		return GF_OK;
	}

private:
	std::vector<char> m_cached;
};

}} // namespace ioremap::nulla

#endif // __NULLA_ISO_READER_HPP
