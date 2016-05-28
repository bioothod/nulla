#ifndef __NULLA_SAMPLE_HPP
#define __NULLA_SAMPLE_HPP

#include <gpac/mpeg4_odf.h>
#include <gpac/setup.h>
#include <gpac/tools.h> // breaks alphabet ordering, since sync_layer.h requires definition of GF_Err
#include <gpac/sync_layer.h>

#include <algorithm>
#include <sstream>
#include <vector>

#include <errno.h>
#include <msgpack.hpp>

namespace ioremap { namespace nulla {

struct sample {
	u32		length = 0;
	u32		di = 0;
	u64		offset = 0;
	u64		dts = 0;
	u64		cts_offset = 0;
	bool		is_rap = false;

	bool operator<(const sample &other) const {
		return dts < other.dts;
	}

	MSGPACK_DEFINE(length, di, offset, dts, cts_offset, is_rap);
};

ssize_t sample_position_from_dts(const std::vector<sample> &collection, u64 dts, bool want_rap) {
	sample tmp;
	tmp.dts = dts;

	auto it = std::upper_bound(collection.begin(), collection.end(), tmp);
	if (it == collection.end())
		return -E2BIG;

	ssize_t diff = std::distance(collection.begin(), it);
	if (diff <= 0 || diff >= (ssize_t)collection.size())
		return -EINVAL;

	--diff;

	if (want_rap) {
		bool has_rap = false;
		do {
			const sample &sam = collection[diff];
			if (sam.is_rap) {
				has_rap = true;
				break;
			}
		} while (++diff < (ssize_t)collection.size() - 1);
		if (!has_rap) {
			return -ENOENT;
		}

		return diff;
	}

	while (++diff < (ssize_t)collection.size() - 1) {
		const sample &sam = collection[diff];
		if (sam.is_rap) {
			--diff;
			break;
		}
	}

	return diff;
}

struct descriptor {
	u8			tag = 0;
	std::vector<char>	data;

	MSGPACK_DEFINE(tag, data);

	void assign(GF_DefaultDescriptor *desc) {
		data.resize(desc->dataLength);
		memcpy((char *)data.data(), desc->data, desc->dataLength);
		tag = desc->tag;
	}

	void export_data(GF_DefaultDescriptor *dst) const {
		if (data.empty())
			return;

		gf_free(dst->data);
		dst->data = (char *)gf_malloc(data.size());
		if (!dst->data)
			throw std::bad_alloc();
		memcpy(dst->data, data.data(), data.size());
		dst->tag = tag;
		dst->dataLength = data.size();
	}
};

struct decoder_config {
	u8		tag = 0;
	u32		objectTypeIndication = 0;
	u8		streamType = 0;
	u8		upstream = 0;
	u32		bufferSizeDB = 0;
	u32		maxBitrate = 0;
	u32		avgBitrate = 0;

	descriptor	decoderSpecificInfo;

	/*placeholder for RVC decoder config if any*/
	u16		predefined_rvc_config = 0;
	descriptor	rvc_config;

	MSGPACK_DEFINE(tag, objectTypeIndication, streamType, upstream, bufferSizeDB, maxBitrate, avgBitrate,
			decoderSpecificInfo,
			predefined_rvc_config, rvc_config);

	decoder_config() {}
	decoder_config(const GF_DecoderConfig *src) {
		if (!src)
			return;

		tag = src->tag;
		objectTypeIndication = src->objectTypeIndication;
		streamType = src->streamType;
		upstream = src->upstream;
		bufferSizeDB = src->bufferSizeDB;
		maxBitrate = src->maxBitrate;
		avgBitrate = src->avgBitrate;

		if (src->decoderSpecificInfo) {
			decoderSpecificInfo.assign(src->decoderSpecificInfo);
		}

		predefined_rvc_config = src->predefined_rvc_config;
		if (src->rvc_config) {
			rvc_config.assign(src->rvc_config);
		}
	}

	void export_data(GF_DecoderConfig *dst) const {
		dst->tag = tag;
		dst->objectTypeIndication = objectTypeIndication;
		dst->streamType = streamType;
		dst->upstream = upstream;
		dst->bufferSizeDB = bufferSizeDB;
		dst->maxBitrate = maxBitrate;
		dst->avgBitrate = avgBitrate;

		if (dst->decoderSpecificInfo) {
			decoderSpecificInfo.export_data(dst->decoderSpecificInfo);
		}

		dst->predefined_rvc_config = predefined_rvc_config;
		if (dst->rvc_config) {
			rvc_config.export_data(dst->rvc_config);
		}
	}
};

struct esd {
	std::vector<char>	slconf;
	decoder_config		dconf;

	MSGPACK_DEFINE(slconf, dconf);

	esd() {}
	esd(GF_ESD *e) : dconf(e->decoderConfig) {
		slconf.resize(sizeof(GF_SLConfig));
		memcpy((char *)slconf.data(), e->slConfig, sizeof(GF_SLConfig));
	}

	void export_data(GF_ESD *dst) const {
		if (dst->slConfig) {
			if (sizeof(GF_SLConfig) != slconf.size()) {
				std::ostringstream ss;
				ss << "slConfig size mismatch: runtime: " << sizeof(GF_SLConfig) <<
					", msgpack read: " << slconf.size();

				throw std::runtime_error(ss.str());
			}

			memcpy(dst->slConfig, slconf.data(), sizeof(GF_SLConfig));
		}

		if (dst->decoderConfig) {
			dconf.export_data(dst->decoderConfig);
		}
	}

};

struct track {
	enum {
		serialize_version_1 = 1,
	};

	u32		media_type = 0;
	u32		media_subtype = 0;
	u32		media_subtype_mpeg4 = 0;
	u32		media_timescale = 0;
	u64		media_duration = 0;

	std::string	mime_type;
	std::string	codec;

	u32		id = 0;
	u32		number = 0;

	u32		timescale = 0;
	u64		duration = 0;

	u64		data_size = 0;
	u32		bandwidth = 0;

	struct audio {
		u32	sample_rate = 0;
		u32	channels = 0;
		u8	bps = 0; // bits per sample (!), not per second (!!)

		enum {
			serialize_version_1 = 1,
		};

		template <typename Stream>
		void msgpack_pack(msgpack::packer<Stream> &o) const {
			o.pack_array(4);
			o.pack((int)audio::serialize_version_1);
			o.pack(sample_rate);
			o.pack(channels);
			o.pack(bps);
		}

		void msgpack_unpack(msgpack::object o) {
			if (o.type != msgpack::type::ARRAY) {
				std::ostringstream ss;
				ss << "could not unpack audio, object type is " << o.type <<
					", must be array (" << msgpack::type::ARRAY << ")";
				throw std::runtime_error(ss.str());
			}

			int version;

			msgpack::object *p = o.via.array.ptr;
			p[0].convert(&version);

			switch (version) {
			case audio::serialize_version_1:
				p[1].convert(&sample_rate);
				p[2].convert(&channels);
				p[3].convert(&bps);
				break;
			default: {
				std::ostringstream ss;
				ss << "could not unpack audio, invalid version " << version;
				throw std::runtime_error(ss.str());
			}
			}
		}

		std::string str() const {
			std::ostringstream ss;
			if (sample_rate == 0 || channels == 0) {
				ss << "none";
			} else {
				ss << "sample_rate: " << sample_rate
					<< ", channels: " << channels
					<< ", bps: " << (int)bps
					;
			}

			return ss.str();
		}
	} audio;

	struct video {
		u32	width = 0, height = 0;
		u32	fps_num = 0, fps_denum = 0;
		u32	sar_w = 0, sar_h = 0;

		enum {
			serialize_version_1 = 1,
		};

		template <typename Stream>
		void msgpack_pack(msgpack::packer<Stream> &o) const {
			o.pack_array(7);
			o.pack((int)video::serialize_version_1);
			o.pack(width);
			o.pack(height);
			o.pack(fps_num);
			o.pack(fps_denum);
			o.pack(sar_w);
			o.pack(sar_h);
		}

		void msgpack_unpack(msgpack::object o) {
			if (o.type != msgpack::type::ARRAY) {
				std::ostringstream ss;
				ss << "could not unpack video, object type is " << o.type <<
					", must be array (" << msgpack::type::ARRAY << ")";
				throw std::runtime_error(ss.str());
			}

			int version;

			msgpack::object *p = o.via.array.ptr;
			p[0].convert(&version);

			switch (version) {
			case video::serialize_version_1:
				p[1].convert(&width);
				p[2].convert(&height);
				p[3].convert(&fps_num);
				p[4].convert(&fps_denum);
				p[5].convert(&sar_w);
				p[6].convert(&sar_h);
				break;
			default: {
				std::ostringstream ss;
				ss << "could not unpack video, invalid version " << version;
				throw std::runtime_error(ss.str());
			}
			}
		}

		std::string str() const {
			std::ostringstream ss;
			if (width == 0 || height == 0) {
				ss << "none";
			} else {
				ss << "scale: " << width << "x" << height
					<< ", fps_num: " << fps_num
					<< ", fps_denum: " << fps_denum
					<< ", sar: " << sar_w << ":" << sar_h
					;
			}

			return ss.str();
		}
	} video;

	nulla::esd esd;

	std::vector<sample>	samples;

	ssize_t sample_position_from_dts(u64 dts, bool want_rap) const {
		return nulla::sample_position_from_dts(samples, dts, want_rap);
	}

	std::string str() const {
		std::ostringstream ss;

		char mtype_str[16], mstype_str[16], mstype_mpeg4_str[16];
		snprintf(mtype_str, sizeof(mtype_str), "%s", gf_4cc_to_str(media_type));
		snprintf(mstype_str, sizeof(mstype_str), "%s", gf_4cc_to_str(media_subtype));
		snprintf(mstype_mpeg4_str, sizeof(mstype_mpeg4_str), "%s", gf_4cc_to_str(media_subtype_mpeg4));

		ss << "media_type: " << mtype_str
		   << ", media_subtype: " << mstype_str
		   << ", media_subtype_mpeg4: " << mstype_mpeg4_str
		   << ", media_timescale: " << media_timescale
		   << ", media_duration: " << media_duration
		   << ", mime_type: " << mime_type
		   << ", codec: " << codec
		   << ", id: " << id
		   << ", number: " << number
		   << ", timescale: " << timescale
		   << ", duration: " << duration
		   << ", bandwidth: " << bandwidth
		   << ", audio: " << audio.str()
		   << ", video: " << video.str()
		   << ", samples: " << samples.size()
		   ;

		return ss.str();
	}

	template <typename Stream>
	void msgpack_pack(msgpack::packer<Stream> &o) const {
		o.pack_array(18);
		o.pack((int)track::serialize_version_1);
		o.pack(media_type);
		o.pack(media_subtype);
		o.pack(media_subtype_mpeg4);
		o.pack(media_timescale);
		o.pack(media_duration);
		o.pack(mime_type);
		o.pack(codec);
		o.pack(id);
		o.pack(number);
		o.pack(timescale);
		o.pack(duration);
		o.pack(bandwidth);
		o.pack(data_size);
		o.pack(audio);
		o.pack(video);
		o.pack(esd);
		o.pack(samples);
	}

	void msgpack_unpack(msgpack::object o) {
		if (o.type != msgpack::type::ARRAY) {
			std::ostringstream ss;
			ss << "could not unpack track, object type is " << o.type <<
				", must be array (" << msgpack::type::ARRAY << ")";
			throw std::runtime_error(ss.str());
		}

		int version;

		msgpack::object *p = o.via.array.ptr;
		p[0].convert(&version);

		switch (version) {
		case track::serialize_version_1:
			p[1].convert(&media_type);
			p[2].convert(&media_subtype);
			p[3].convert(&media_subtype_mpeg4);
			p[4].convert(&media_timescale);
			p[5].convert(&media_duration);
			p[6].convert(&mime_type);
			p[7].convert(&codec);
			p[8].convert(&id);
			p[9].convert(&number);
			p[10].convert(&timescale);
			p[11].convert(&duration);
			p[12].convert(&bandwidth);
			p[13].convert(&data_size);
			p[14].convert(&audio);
			p[15].convert(&video);
			p[16].convert(&esd);
			p[17].convert(&samples);
			break;
		default: {
			std::ostringstream ss;
			ss << "could not unpack track, invalid version " << version;
			throw std::runtime_error(ss.str());
		}
		}
	}
};


}} // namespace ioremap::nulla

#endif //__NULLA_SAMPLE_HPP

