#ifndef __NULLA_SAMPLE_HPP
#define __NULLA_SAMPLE_HPP

#include <gpac/setup.h>
#include <gpac/tools.h>

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

ssize_t sample_position_from_dts(const std::vector<sample> &collection, u64 dts) {
	sample tmp;
	tmp.dts = dts;

	auto it = std::upper_bound(collection.begin(), collection.end(), tmp);
	if (it == collection.end())
		return -E2BIG;

	ssize_t diff = std::distance(collection.begin(), it);
	if (diff <= 0 || diff >= (ssize_t)collection.size())
		return -EINVAL;

	return diff - 1;
}

struct track {
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

	u32		bandwidth = 0;

	struct {
		u32	sample_rate = 0;
		u32	channels = 0;

		MSGPACK_DEFINE(sample_rate, channels);

		std::string str() const {
			std::ostringstream ss;
			if (sample_rate == 0 || channels == 0) {
				ss << "none";
			} else {
				ss << "sample_rate: " << sample_rate
					<< ", channels: " << channels
					;
			}

			return ss.str();
		}
	} audio;

	struct {
		u32	width = 0, height = 0;
		u32	fps_num = 0, fps_denum = 0;
		u32	sar_w = 0, sar_h = 0;

		MSGPACK_DEFINE(width, height, fps_num, fps_denum, sar_w, sar_h);

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

	std::vector<sample>	samples;

	MSGPACK_DEFINE(media_type, media_subtype, media_subtype_mpeg4, media_timescale, media_duration,
			mime_type, codec,
			id, number,
			timescale, duration,
			bandwidth,
			audio, video,
			samples);

	ssize_t sample_position_from_dts(u64 dts) const {
		return nulla::sample_position_from_dts(samples, dts);
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
};


}} // namespace ioremap::nulla

#endif //__NULLA_SAMPLE_HPP

