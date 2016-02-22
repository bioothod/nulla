#ifndef __NULLA_ISO_WRITER_HPP
#define __NULLA_ISO_WRITER_HPP

#include "nulla/sample.hpp"

#include <fstream>

#include <gpac/tools.h>
#include <gpac/isomedia.h>

namespace ioremap { namespace nulla {

struct writer_options {
	u64		dts_start, dts_end;
	ssize_t		pos_start, pos_end;

	u32		fragment_duration;

	u64		dts_start_absolute;

	const char	*sample_data;
	size_t		sample_data_size;
};

class iso_writer {
public:
	iso_writer(const track &tr) : m_track(tr) {}

	int create(const writer_options &opt, std::vector<char> &movie_data) {
		GF_ISOFile *movie;
		GF_ESD *esd;
		GF_Err e;
		u32 track, di;
		char filename[128];
		u32 fragment_duration = 0;
		u64 start_range;

		snprintf(filename, sizeof(filename), "/tmp/test-%016ld.mp4", (unsigned long)opt.dts_start);
		unlink(filename);
		movie = gf_isom_open(filename, GF_ISOM_OPEN_WRITE, NULL);
		if (!movie) {
			e = gf_isom_last_error(NULL);
			goto err_out_exit;
		}

		track = gf_isom_new_track(movie, m_track.number, m_track.media_type, m_track.timescale);
		gf_isom_set_track_enabled(movie, m_track.number, 1);
		esd = gf_odf_desc_esd_new(SLPredef_MP4);
		esd->decoderConfig->streamType = (m_track.media_type == GF_ISOM_MEDIA_AUDIO) ? GF_STREAM_AUDIO : GF_STREAM_VISUAL;
		gf_isom_new_mpeg4_description(movie, track, esd, NULL, NULL, &di);

		if (m_track.media_subtype == GF_ISOM_SUBTYPE_MPEG4) {
			e = gf_isom_set_media_subtype(movie, m_track.number, di, m_track.media_subtype_mpeg4);
			if (e != GF_OK) {
				printf("could not set media subtype %s (%08x): %d\n",
						gf_4cc_to_str(m_track.media_subtype_mpeg4), m_track.media_subtype_mpeg4, e);
				goto err_out_free_movie;
			}
		} else {
			e = gf_isom_set_media_subtype(movie, m_track.number, di, m_track.media_subtype);
			if (e != GF_OK) {
				printf("could not set media subtype %s (%08x): %d\n",
						gf_4cc_to_str(m_track.media_subtype), m_track.media_subtype, e);
				goto err_out_free_movie;
			}
		}

		e = gf_isom_setup_track_fragment(movie, m_track.number, di, 0, 0, 0, 0, 0);
		if (e != GF_OK) {
			printf("could not setup track fragment: %d\n", e);
			goto err_out_free_movie;
		}

		e = gf_isom_finalize_for_fragment(movie, 2);
		if (e != GF_OK) {
			printf("could not finalize for fragmentation: %d\n", e);
			goto err_out_free_movie;
		}

		start_range = gf_isom_get_file_size(movie);

		e = gf_isom_start_segment(movie, NULL, GF_TRUE);
		if (e != GF_OK) {
			printf("could not start segment fragment: %d\n", e);
			goto err_out_free_movie;
		}


		GF_ISOSample samp;

		u32 duration;
		for (ssize_t i = opt.pos_start; i <= opt.pos_end; ++i) {
			const sample &ms = m_track.samples[i];

			if ((fragment_duration == 0) || (fragment_duration > opt.fragment_duration && ms.is_rap)) {
				e = gf_isom_start_fragment(movie, GF_TRUE);
				if (e) {
					printf("%zd/%zd, track: %d, fragment_duration: %d/%d could not start fragment: err: %d\n",
						i, opt.pos_end, m_track.number, fragment_duration, opt.fragment_duration, e);
					goto err_out_free_movie;
				}

				fragment_duration = 0;

				gf_isom_set_traf_base_media_decode_time(movie, m_track.number, opt.dts_start_absolute + ms.dts);

				printf("%zd/%zd, fragment started, track: %d, di: %x, length: %d, rap: %d, "
						"dts: %lu, ms.dts: %lu, first.dts: %lu, duration: %u, cts: %lu, "
						"offset: %zd\n",
						i, opt.pos_end, m_track.number, di, ms.length, ms.is_rap,
						(unsigned long)ms.dts - m_track.samples[opt.pos_start].dts,
						(unsigned long)ms.dts, (unsigned long)m_track.samples[opt.pos_start].dts,
						duration,
						(unsigned long)ms.cts_offset,
						ms.offset - m_track.samples[opt.pos_start].offset);
			}

			size_t offset = ms.offset - m_track.samples[opt.pos_start].offset;
			if (offset >= opt.sample_data_size) {
				e = (GF_Err)-E2BIG;
				goto check;
			}

			if (ms.length + offset > opt.sample_data_size) {
				e = (GF_Err)-EINVAL;
				goto check;
			}

			samp.dataLength = ms.length;
			samp.data = (char *)opt.sample_data + offset;
			samp.IsRAP = ms.is_rap ? RAP : RAP_NO;
			samp.DTS = ms.dts - m_track.samples[opt.pos_start].dts;
			samp.CTS_Offset = ms.cts_offset;

			if (i < (ssize_t)m_track.samples.size() - 1) {
				duration = m_track.samples[i + 1].dts - ms.dts;
			}

			e = gf_isom_fragment_add_sample(movie, track, &samp, di, duration, 0, 0, GF_FALSE);
			if (e) {
				printf("%zd/%zd, track: %d, di: %x, length: %d, rap: %d, dts: %lu, "
					"offset: %zd, sample_data_size: %zd, could not add sample: err: %d\n",
					i, opt.pos_end, m_track.number, di, samp.dataLength, samp.IsRAP, (unsigned long)samp.DTS,
					offset, opt.sample_data_size, e);
				goto check;
			}

			fragment_duration += duration;

check:
#if 1
			if (i == opt.pos_end) {
				printf("%zd/%zd, added sample, track: %d, di: %x, length: %d, rap: %d, "
						"dts: %lu, ms.dts: %lu, first.dts: %lu, duration: %u, cts: %lu, "
						"offset: %zd, err: %d\n",
						i, opt.pos_end, m_track.number, di, samp.dataLength, samp.IsRAP,
						(unsigned long)samp.DTS,
						(unsigned long)ms.dts, (unsigned long)m_track.samples[opt.pos_start].dts,
						duration,
						(unsigned long)samp.CTS_Offset,
						offset, e);
			}
#endif
			if (e)
				goto err_out_free_movie;

		}

		e = gf_isom_close_fragments(movie);
		gf_isom_update_duration(movie);

		e = gf_isom_close(movie);
		if (e) {
			return e;
		}

		{
			movie_data.resize(gf_isom_get_file_size(movie) - start_range);
			std::ifstream in(filename, std::ifstream::binary);
			in.seekg(start_range);
			in.read((char *)movie_data.data(), movie_data.size());
		}

		return 0;

err_out_free_movie:
		gf_isom_delete(movie);
err_out_exit:
		return (int)e;
	}

private:
	track m_track;
};

}} // namespace ioremap::nulla

#endif //__NULLA_ISO_WRITER_HPP
