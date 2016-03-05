#ifndef __NULLA_HLS_PLAYLIST_HPP
#define __NULLA_HLS_PLAYLIST_HPP

#include "nulla/playlist.hpp"

#include <sstream>

namespace ioremap { namespace nulla {

class m3u8 {
public:
	m3u8(const nulla::playlist_t &playlist) : m_playlist(playlist) {
	}

	void generate() {
		m_ss << "#EXTM3U\n";
		m_ss << "#EXT-X-VERSION:3\n";
		m_ss << "#EXT-X-MEDIA-SEQUENCE:0\n";

		BOOST_FOREACH(const nulla::period &pr, m_playlist->periods) {
			m_total_duration_msec += pr.duration_msec;
			if (pr.duration_msec > m_max_segment_duration_msec)
				m_max_segment_duration_msec = pr.duration_msec;
		}

		m_ss << "#EXT-X-TARGETDURATION:" << m_max_segment_duration_msec / 1000 << "\n";

		int adaptation_offset = 0;
		for (auto it = m_playlist->periods.begin(), it_end = m_playlist->periods.end(); it != it_end;) {
			add_period(adaptation_offset, *it);
			adaptation_offset += it->adaptations.size();

			++it;
			if (it == it_end) {
				break;
			}

			m_ss << "#EXT-X-DISCONTINUITY\n";
		}
	}

	std::string main_playlist() const {
		return m_ss.str();
	}

	std::string variant_playlist(const std::string &prefix) const {
		auto it = m_variants.find(prefix);
		if (it == m_variants.end())
			return "";
		return it->second;
	}

private:
	nulla::playlist_t m_playlist;
	long m_total_duration_msec = 0;
	long m_max_segment_duration_msec = 0;
	std::ostringstream m_ss;
	std::map<std::string, std::string> m_variants;

	void add_period(int adaptation_offset, const nulla::period &p) {
		int pos = 1 + adaptation_offset;

		BOOST_FOREACH(const nulla::adaptation &aset, p.adaptations) {
			std::string adaptation_id = std::to_string(pos);
			add_aset(adaptation_id, aset);
			pos++;
		}
	}

	void add_aset(const std::string &adaptation_id, const nulla::adaptation &a) {
		BOOST_FOREACH(const std::string &id, a.repr_ids) {
			auto it = m_playlist->repr.find(id);
			if (it == m_playlist->repr.end())
				continue;

			add_representation(m_ss, adaptation_id, it->second);
		}
	}

	void add_representation(std::ostringstream &ss, const std::string &adaptation_id, const nulla::representation &r) {
		const nulla::track_request &trf = r.tracks.front();
		const nulla::track &track = trf.track();

		std::string variant = r.id;
		std::string url = m_playlist->base_url + "playlist/" + variant;

		std::string codec = track.codec;
		if (codec.substr(0, 4) == "avc3") {
			codec[3] = '1';
		}

		ss << "#EXT-X-STREAM-INF:PROGRAM-ID=" << adaptation_id
			<< ",BANDWIDTH=" << track.bandwidth
			<< ",CODECS=\"" << codec << "\""
		;

		if (track.media_type == GF_ISOM_MEDIA_VISUAL) {
			ss << ",RESOLUTION=" << track.video.width << "x" << track.video.height;
		}

		ss << "\n" << url << "\n";

		std::ostringstream pls;
		pls	<< "#EXTM3U\n"
			<< "#EXT-X-VERSION:3\n"
			<< "#EXT-X-PLAYLIST-TYPE:VOD\n"
			<< "#EXT-X-MEDIA-SEQUENCE:0\n"
			<< "#EXT-X-TARGETDURATION:" << m_playlist->chunk_duration_sec << "\n"
		;

		for (const nulla::track_request &tr: r.tracks) {
			float total_duration = 0;
			float track_duration = (float)tr.duration_msec / 1000.0;

			int track_in_msec = 1000 * m_playlist->chunk_duration_sec;
			int number_max = (tr.duration_msec + track_in_msec - 1) / track_in_msec;

			float duration = m_playlist->chunk_duration_sec;

			for (int i = 0; i < number_max; ++i) {
				if (i == number_max - 1)
					duration = track_duration - total_duration;

				pls	<< "#EXTINF:" << duration << ",\n"
					<< m_playlist->base_url + "play/" + r.id + "/" + std::to_string(tr.start_number + i) << "\n"
				;

				total_duration += duration;
			}
		}
		pls << "#EXT-X-ENDLIST";

		m_variants[variant] = pls.str();
	}
};

}} // namespace ioremap::nulla

#endif // __NULLA_HLS_PLAYLIST_HPP
