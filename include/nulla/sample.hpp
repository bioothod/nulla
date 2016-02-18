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
	u32		length;
	u32		di;
	u64		offset;
	u64		dts;
	u64		cts_offset;
	bool		is_rap;

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
	u32		media_type;
	u32		media_subtype;
	u32		media_subtype_mpeg4;
	u32		stream_type;

	u32		id;
	u32		number;

	u32		timescale;
	u64		duration;

	std::vector<sample>	samples;

	MSGPACK_DEFINE(media_type, media_subtype, media_subtype_mpeg4, stream_type, id, number, timescale, duration, samples);

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
		   << ", stream_type: " << stream_type
		   << ", id: " << id
		   << ", number: " << number
		   << ", timescale: " << timescale
		   << ", duration: " << duration
		   << ", samples: " << samples.size()
		   ;

		return ss.str();
	}
};


}} // namespace ioremap::nulla

#endif //__NULLA_SAMPLE_HPP

