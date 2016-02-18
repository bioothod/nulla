#ifndef __NULLA_ISO_READER_HPP
#define __NULLA_ISO_READER_HPP

#include "nulla/sample.hpp"

#include <gpac/constants.h>
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
};

class iso_reader {
public:
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
	~iso_reader() {
		gf_isom_close(m_movie);
		gf_sys_close();
	}

	int parse() {
		GF_Err e;

		// tracks start from 1
		for (u32 i = 1; i <= gf_isom_get_track_count(m_movie); i++) {
			nulla::track t;

			t.number = i;
			t.media_type = gf_isom_get_media_type(m_movie, i);
			t.media_subtype = gf_isom_get_media_subtype(m_movie, i, 1);
			t.media_subtype_mpeg4 = gf_isom_get_mpeg4_subtype(m_movie, i, 1);
			t.id = gf_isom_get_track_id(m_movie, i);
			t.duration = gf_isom_get_track_duration(m_movie, i);
			t.timescale = gf_isom_get_media_timescale(m_movie, i);

			GF_ESD *esd = gf_isom_get_esd(m_movie, i, 1);
			if (!esd) {
				t.stream_type = GF_STREAM_OD;
			} else {
				t.stream_type = esd->decoderConfig->streamType;
				gf_odf_desc_del((GF_Descriptor *) esd);
			}

			e = parse_track(t);
			if (e == GF_OK) {
				std::cout << "parsed track: " << t.str() << std::endl; 
				m_media.tracks.emplace_back(t);
			}
		}

		return 0;
	}

	std::string pack() const {
		std::stringstream buffer;
		msgpack::pack(buffer, m_media);
		buffer.seekg(0);

		return buffer.str();
	}

private:
	GF_ISOFile *m_movie;
	media m_media;

	GF_Err parse_track(track &t) {
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
			/* let's analyze the samples we have parsed so far one by one */
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

			/*here we dump some sample info: samp->data, samp->dataLength, samp->isRAP, samp->DTS, samp->CTS_Offset */
#if 1
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


}} // namespace ioremap::nulla

namespace msgpack {
static inline ioremap::nulla::media &operator >>(msgpack::object o, ioremap::nulla::media &meta)
{
	if (o.type != msgpack::type::ARRAY) {
		std::ostringstream ss;
		ss << "page unpack: type: " << o.type <<
			", must be: " << msgpack::type::ARRAY <<
			", size: " << o.via.array.size;
		throw std::runtime_error(ss.str());
	}

	object *p = o.via.array.ptr;
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

		p[1].convert(&meta.tracks);
		break;
	}
	default: {
		std::ostringstream ss;
		ss << "page unpack: version mismatch: read: " << version <<
			", there is no such packing version ";
		throw std::runtime_error(ss.str());
	}
	}

	return meta;
}

template <typename Stream>
inline msgpack::packer<Stream> &operator <<(msgpack::packer<Stream> &o, const ioremap::nulla::media &meta)
{
	o.pack_array(ioremap::nulla::media::serialization_version_2);
	o.pack((int)ioremap::nulla::media::serialization_version_2);
	o.pack(meta.tracks);

	return o;
}

} // namespace msgpack


#endif // __NULLA_ISO_READER_HPP
