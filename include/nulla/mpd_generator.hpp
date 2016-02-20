#ifndef __NULLA_MPD_GENERATOR_HPP
#define __NULLA_MPD_GENERATOR_HPP

#include "nulla/playlist.hpp"

#include <boost/foreach.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <sstream>
#include <string>

namespace ioremap { namespace nulla {

namespace {
	namespace pt = boost::property_tree;
}

class mpd {
public:
	mpd(const nulla::playlist_t &playlist) : m_playlist(playlist) {
	}

	void generate() {
		pt::ptree mpd;
		mpd.put("<xmlattr>.xmlns", "urn:mpeg:dash:schema:mpd:2011");
		mpd.put("<xmlattr>.minBufferTime", "PT1.500S");
		mpd.put("<xmlattr>.profiles", "urn:mpeg:dash:profile:full:2011");
		mpd.put("<xmlattr>.type", "static");

		mpd.put("BaseURL", m_playlist->base_url);

		BOOST_FOREACH(const nulla::period &pr, m_playlist->periods) {
			add_period(mpd, pr);
		}

		mpd.put("<xmlattr>.mediaPresentationDuration", print_time(m_total_duration_msec));
		mpd.put("<xmlattr>.maxSegmentDuration", print_time(m_max_segment_duration_msec));
		m_root.add_child("MPD", mpd);
	}

	std::string xml() const {
		std::ostringstream ss;
		pt::write_xml(ss, m_root);
		return ss.str();
	}


private:
	nulla::playlist_t m_playlist;
	pt::ptree m_root;
	long m_total_duration_msec = 0;
	long m_max_segment_duration_msec = 0;

	void add_period(pt::ptree &mpd, const nulla::period &p) {
		pt::ptree period;

		m_total_duration_msec += p.duration_msec;
		if (p.duration_msec > m_max_segment_duration_msec)
			m_max_segment_duration_msec = p.duration_msec;

		period.put("<xmlattr>.duration", print_time(p.duration_msec));

		BOOST_FOREACH(const nulla::adaptation &aset, p.adaptations) {
			add_aset(period, aset);
		}

		mpd.add_child("Period", period);
	}

	void add_aset(pt::ptree &period, const nulla::adaptation &a) {
		pt::ptree aset;

		aset.put("<xmlattr>.segmentAlignment", "true");
		BOOST_FOREACH(const std::string &id, a.repr_ids) {
			auto it = m_playlist->repr.find(id);
			if (it == m_playlist->repr.end())
				continue;
			
			add_representation(aset, it->second);
		}

		period.add_child("AdaptationSet", aset);
	}

	void add_representation(pt::ptree &aset, const nulla::representation &r) {
		pt::ptree repr;

		repr.put("<xmlattr>.id", r.id);
		repr.put("<xmlattr>.startWithSAP", "1");

		// we generate MPD based on the very first track request
		// it is not allowed to change codec or bandwidth for example in the subsequent
		// track requests
		//
		// each track request can have multiple tracks, we are only interested in the @requested_track_number
		// this number can differ in each track requested, but all selected tracks among all track requests
		// should have the same nature, i.e. do not require reinitialization

		const nulla::track_request &trf = r.tracks.front();
		printf("%s: id: %s, duration: %ld, track-requests: %ld, trf: requested_track_number: %d, tracks: %ld\n",
				__func__, r.id.c_str(), r.duration_msec, r.tracks.size(),
				trf.requested_track_number, trf.media.tracks.size());

		BOOST_FOREACH(const nulla::track &track, trf.media.tracks) {
			printf("%s: requested_track_number: %d, track-number: %d, track: %s\n",
					__func__, trf.requested_track_number, track.number, track.str().c_str());

			if (track.number == trf.requested_track_number) {
				repr.put("<xmlattr>.mimeType", track.mime_type);
				repr.put("<xmlattr>.codecs", track.codec);
				repr.put("<xmlattr>.bandwidth", track.bandwidth);

				if (track.media_type == GF_ISOM_MEDIA_AUDIO) {
					repr.put("<xmlattr>.audioSamplingRate", track.audio.sample_rate);

					pt::ptree channel;
					channel.put("<xmlattr>.schemeIdUri", "urn:mpeg:dash:23003:3:audio_channel_configuration:2011");
					channel.put("<xmlattr>.value", track.audio.channels);

					repr.add_child("AudioChannelConfiguration", channel);
				} else if (track.media_type == GF_ISOM_MEDIA_VISUAL) {
					u32 fps_num = track.video.fps_num;
					u32 fps_denum = track.video.fps_denum;

					gf_media_get_reduced_frame_rate(&fps_num, &fps_denum);

					repr.put("<xmlattr>.width", track.video.width);
					repr.put("<xmlattr>.height", track.video.height);
					if (fps_denum > 1) {
						repr.put("<xmlattr>.frameRate",
								std::to_string(track.video.fps_num) + "/" +
								std::to_string(track.video.fps_denum));
					} else {
						repr.put("<xmlattr>.frameRate", track.video.fps_num);
					}
					repr.put("<xmlattr>.sar",
							std::to_string(track.video.sar_w) + ":" +
							std::to_string(track.video.sar_h));
				}

				break;
			}
		}

		aset.add_child("Representation", repr);
	}

	std::string print_time(long duration_msec) const {
		float sec = (float)duration_msec / 1000.0;
		int h = sec / 3600;
		int m = (sec - h * 3600) / 60;
		float s = sec - h * 3600 - m * 60;
		char tmp[36];
		snprintf(tmp, sizeof(tmp), "PT%dH%dM%.3fS", h, m, s);
		return std::string(tmp);
	}
};

}} // namespace ioremap::nulla

#endif // __NULLA_MPD_GENERATOR_HPP
