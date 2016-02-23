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

	int create(const writer_options &opt, bool init, std::vector<char> &init_data, std::vector<char> &movie_data) {
		GF_ISOFile *movie;
		GF_ESD *esd;
		GF_Err e;
		u32 di;
		char filename[128];
		u32 fragment_duration = 0;
		u64 start_range;
		u32 duration = 0;
		u32 track_id = 1;
		u32 track_number;

		snprintf(filename, sizeof(filename), "/tmp/test-%016ld.mp4", (unsigned long)opt.dts_start);
		unlink(filename);
		movie = gf_isom_open(filename, GF_ISOM_OPEN_WRITE, NULL);
		if (!movie) {
			e = gf_isom_last_error(NULL);
			goto err_out_exit;
		}

		track_number = gf_isom_new_track(movie, track_id, m_track.media_type, m_track.media_timescale);
		gf_isom_set_track_enabled(movie, track_number, 1);
		esd = gf_odf_desc_esd_new(SLPredef_MP4);
		m_track.esd.export_data(esd);
		gf_isom_new_mpeg4_description(movie, track_number, esd, NULL, NULL, &di);

		if (m_track.media_subtype == GF_ISOM_SUBTYPE_MPEG4) {
			e = gf_isom_set_media_subtype(movie, track_number, di, m_track.media_subtype_mpeg4);
			if (e != GF_OK) {
				printf("could not set media subtype %s (%08x): %d\n",
						gf_4cc_to_str(m_track.media_subtype_mpeg4), m_track.media_subtype_mpeg4, e);
				goto err_out_free_movie;
			}
		} else {
			e = gf_isom_set_media_subtype(movie, track_number, di, m_track.media_subtype);
			if (e != GF_OK) {
				printf("could not set media subtype %s (%08x): %d\n",
						gf_4cc_to_str(m_track.media_subtype), m_track.media_subtype, e);
				goto err_out_free_movie;
			}
		}

		if (m_track.media_type == GF_ISOM_MEDIA_VISUAL) {
			gf_isom_set_visual_info(movie, track_number, di, m_track.video.width, m_track.video.height);
#if 0
			e = gf_isom_avc_set_inband_config(movie, track_number, di);
			if (e != GF_OK) {
				printf("could not set inband config: %d\n", e);
				goto err_out_free_movie;
			}
#endif
		}
		if (m_track.media_type == GF_ISOM_MEDIA_AUDIO) {
			e = gf_isom_set_audio_info(movie, track_number, di,
					m_track.audio.sample_rate, m_track.audio.channels, m_track.audio.bps);
			if (e != GF_OK) {
				printf("could not setup audio info: %d\n", e);
				goto err_out_free_movie;
			}
		}

		e = gf_isom_setup_track_fragment(movie, track_id, di, 0, 0, 0, 0, 0);
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
		if (init) {
			e = gf_isom_close(movie);

			init_data.resize(start_range);
			std::ifstream in(filename, std::ifstream::binary);
			in.seekg(0);
			in.read((char *)init_data.data(), init_data.size());
			return e;
		}


		e = gf_isom_start_segment(movie, NULL, GF_TRUE);
		if (e != GF_OK) {
			printf("could not start segment fragment: %d\n", e);
			goto err_out_free_movie;
		}


		GF_ISOSample samp;

		for (ssize_t i = opt.pos_start; i <= opt.pos_end; ++i) {
			const sample &ms = m_track.samples[i];

			if (i < (ssize_t)m_track.samples.size() - 1) {
				duration = m_track.samples[i + 1].dts - ms.dts;
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


			if ((fragment_duration == 0) || (fragment_duration > opt.fragment_duration && ms.is_rap)) {
				e = gf_isom_start_fragment(movie, GF_TRUE);
				if (e) {
					printf("%zd/%zd, track_number: %d, track_id: %d, "
							"fragment_duration: %d/%d could not start fragment: err: %d\n",
						i, opt.pos_end, track_number, track_id, fragment_duration, opt.fragment_duration, e);
					goto err_out_free_movie;
				}

				fragment_duration = 0;

				gf_isom_set_traf_base_media_decode_time(movie, track_id, opt.dts_start_absolute + ms.dts);

				printf("%zd/%zd, fragment started, track_number: %d, track_id: %d, di: %x, length: %d, rap: %d, di: %d, "
						"dts_start_absolute: %lu, dts: %lu, ms.dts: %lu, first.dts: %lu, duration: %u, cts: %lu, "
						"offset: %zd\n",
						i, opt.pos_end, track_number, track_id, di, ms.length, ms.is_rap, ms.di,
						(unsigned long)opt.dts_start_absolute,
						(unsigned long)ms.dts - m_track.samples[opt.pos_start].dts,
						(unsigned long)ms.dts, (unsigned long)m_track.samples[opt.pos_start].dts,
						duration,
						(unsigned long)ms.cts_offset,
						offset);
			}

			samp.dataLength = ms.length;
			samp.data = (char *)opt.sample_data + offset;
			samp.IsRAP = ms.is_rap ? RAP : RAP_NO;
			samp.DTS = ms.dts - m_track.samples[opt.pos_start].dts;
			samp.CTS_Offset = ms.cts_offset;

			e = gf_isom_fragment_add_sample(movie, track_id, &samp, ms.di, duration, 0, 0, GF_FALSE);
			if (e) {
				printf("%zd/%zd, track_number: %d, track_id: %d, di: %x, length: %d, rap: %d, dts: %lu, "
					"offset: %zd, sample_data_size: %zd, could not add sample: err: %d\n",
					i, opt.pos_end, track_number, track_id, di, samp.dataLength, samp.IsRAP, (unsigned long)samp.DTS,
					offset, opt.sample_data_size, e);
				goto check;
			}

			fragment_duration += duration;

check:
#if 1
			if (i == opt.pos_end) {
				printf("%zd/%zd, added sample, track_number: %d, track_id: %d, di: %x, length: %d, rap: %d, "
						"dts: %lu, ms.dts: %lu, first.dts: %lu, duration: %u, cts: %lu, "
						"offset: %zd, err: %d\n",
						i, opt.pos_end, track_number, track_id, di, samp.dataLength, samp.IsRAP,
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

		gf_isom_flush_fragments(movie, GF_TRUE);
		e = gf_isom_close_fragments(movie);
		u64 range_start, range_end;
		e = gf_isom_close_segment(movie, 0, track_id, 0, 0, 0, GF_FALSE, GF_TRUE, 0, &range_start, &range_end);
		gf_isom_update_duration(movie);

		{
			movie_data.resize(gf_isom_get_file_size(movie) - start_range);
			std::ifstream in(filename, std::ifstream::binary);
			in.seekg(start_range);
			in.read((char *)movie_data.data(), movie_data.size());
		}

		e = gf_isom_close(movie);
		if (e) {
			return e;
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
