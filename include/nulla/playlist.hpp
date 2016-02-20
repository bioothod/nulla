#ifndef __NULLA_PLAYLIST_HPP
#define __NULLA_PLAYLIST_HPP

#include "nulla/iso_reader.hpp"

#include <elliptics/session.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace ioremap { namespace nulla {

struct track_request {
	std::string		bucket;
	std::string		key;

	long			start_msec;
	long			duration_msec;

	nulla::media		media;
	u32			requested_track_number; // desired track number among all tracks in given media

	// index of the requested track amond all @media.tracks
	// if @meta_unpack() - playlist generation time - fails to find requested track, it will return error
	// and connection will be reset, so at time when client starts requesting media samples this index
	// will be guaranteed to be valid
	int			requested_track_index = -1;

	elliptics::data_pointer	sample_data;

	const nulla::track &track() const {
		if (requested_track_index == -1) {
			elliptics::throw_error(-EINVAL, "track_request doesn't have valid @requested_track_index");
		}

		return media.tracks[requested_track_index];
	}
};

struct representation {
	std::string			id;

	struct {
		std::string		bucket;
		std::string		key;
	} init;


	long				duration_msec = 0;

	// multiple file requests come one after another to form a continuous stream of movies/songs
	// but from MPD/client point of view it is the same single movie being played even if samples
	// are actually from some other file
	std::vector<track_request>	tracks;
};

struct adaptation {
	// these ids can be used to locate representation in playlist's repr map
	std::vector<std::string>	repr_ids;
};

struct period {
	std::vector<adaptation>		adaptations;
	long				duration_msec = 0;
};

struct raw_playlist {
	// number of track metadata already read and unpacked, it must be atomic since data is being read
	// in parallel and can be unpacked in parallel client IO threads
	std::atomic_int				meta_chunks_read;
	int					meta_chunks_requested = 0;

	int					chunk_duration_sec = 10;

	std::vector<period>			periods;

	// random string
	std::string				cookie;

	// http://endpoint[:port]/@cookie
	std::string				base_url;

	std::map<std::string, representation>	repr;
};

typedef std::shared_ptr<raw_playlist> playlist_t;

}} // namespace ioremap::nulla

#endif // __NULLA_PLAYLIST_HPP
