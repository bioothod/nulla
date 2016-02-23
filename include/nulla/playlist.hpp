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

	// @start_msec in dts units - time position within track when given track should start be played from
	long			dts_start;

	// this is the dts offset of the very first sample of given track among all track requests in given representation
	// track requests will be played one after another and we have to maintain steady timings,
	// thus any subsequent track request has to start when previous one has endedin dts units
	long			dts_first_sample_offset;


	// start number of this track in the representation,
	// "number" is a number of the chunk (@playlist.chunk_duration_sec) to read
	// client asks for chunk #28 (started from 0), but if the first track has duration of 272 seconds,
	// than chunk #28 is the first chunk of the second track. @start_number will store 28 in this example.
	long			start_number;

	// time in milliseconds within given track when it should start be played from
	long			start_msec;
	// duration of the portion of the track to be played in milliseconds
	long			duration_msec;

	nulla::media		media;

	// desired track number among all tracks in given media
	u32			requested_track_number;

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

	// returns iterator pointing to
	std::vector<track_request>::const_iterator find_track_request(int number) const {
		typename std::vector<track_request>::const_iterator it, first, last;
		first = tracks.begin();
		last = tracks.end();

		typename std::vector<track_request>::difference_type count, step;
		count = std::distance(first, last);

		while (count > 0) {
			it = first;
			step = count / 2;
			std::advance(it, step);

			if (it->start_number <= number) {
				first = ++it;
				count -= step + 1;
			} else {
				count = step;
			}
		}

		// we have to return iterator pointing to the track_request which contains requested @dts_start
		// code above implements std::upper_bound() search
		// now we have to decrement iterator
		//
		// if iterator points to the very first track_request, this means that the first track request
		// starts behind requested @dts_start and search has failed
		if (first == tracks.begin())
			return tracks.end();

		std::advance(first, -1);
		return first;
	}
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
