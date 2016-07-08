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

		for (const auto &repr_pair: m_playlist->repr) {
			const nulla::representation &repr = repr_pair.second;

			if (repr.tracks.empty())
				continue;

			add_representation(m_ss, repr);
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
	std::ostringstream m_ss;
	std::map<std::string, std::string> m_variants;

	void add_representation(std::ostringstream &ss, const nulla::representation &r) {
		const nulla::track_request &trf = r.tracks.front();
		const nulla::track &track = trf.track();

		std::string url = m_playlist->base_url + "playlist/" + r.id;

		std::string codec = track.codec;
		if (codec.substr(0, 4) == "avc3") {
			codec[3] = '1';
		}

		ss << "#EXT-X-STREAM-INF"
			":PROGRAM-ID=1" << 
			",BANDWIDTH=" << track.bandwidth <<
			",CODECS=\"" << codec << "\""
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

		m_variants[r.id] = pls.str();
	}
};

}} // namespace ioremap::nulla

#endif // __NULLA_HLS_PLAYLIST_HPP
