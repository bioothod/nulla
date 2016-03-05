#ifndef __NULLA_MPEG2TS_WRITER_HPP
#define __NULLA_MPEG2TS_WRITER_HPP

#include "nulla/sample.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/pixfmt.h>
#include <libavutil/timestamp.h>
}

namespace ioremap { namespace nulla {

class output_stream {
public:
	output_stream(const track &tr, AVFormatContext *oc) : m_oc(oc) {
		enum AVCodecID codec_id = get_codec_id(tr);

		m_codec = avcodec_find_encoder(codec_id);
		if (!m_codec) {
			elliptics::throw_error(-ENOENT, "could not find encoder for '%s'\n", avcodec_get_name(codec_id));
		}

		m_stream = avformat_new_stream(oc, m_codec);
		if (!m_stream) {
			elliptics::throw_error(-ENOMEM, "could not allocate stream for codec '%s'\n", avcodec_get_name(codec_id));
		}

		setup_stream(tr, codec_id);
	}

	~output_stream() {
		for (auto f : m_filters) {
			av_bitstream_filter_close(f);
		}
	}

	int open() {
		AVCodecContext *c = m_stream->codec;
		return avcodec_open2(c, m_codec, NULL);
	}

	void rescale_ts(AVPacket *pkt) {
		AVCodecContext *c = m_stream->codec;

		if (m_codec->type == AVMEDIA_TYPE_VIDEO) {
			av_packet_rescale_ts(pkt, c->time_base, m_stream->time_base);
		}
	}

	int apply_filters(AVPacket *pkt) {
		AVCodecContext *c = m_stream->codec;

		if (m_filters.empty())
			return 0;

		return this->av_apply_bitstream_filters(c, pkt, m_filters[0]);
	}

	int stream_index() const {
		return m_stream->index;
	}

private:
	AVFormatContext *m_oc;
	AVCodec *m_codec = NULL;
	AVStream *m_stream = NULL;

	std::vector<AVBitStreamFilterContext *> m_filters;

	// copied from ffmpeg 3.0.0 where it is public function
	int av_apply_bitstream_filters(AVCodecContext *codec, AVPacket *pkt, AVBitStreamFilterContext *bsfc) {
		int ret = 0;
		while (bsfc) {
			AVPacket new_pkt = *pkt;
			int a = av_bitstream_filter_filter(bsfc, codec, NULL, &new_pkt.data, &new_pkt.size,
					pkt->data, pkt->size, pkt->flags & AV_PKT_FLAG_KEY);
			if (a == 0 && new_pkt.data != pkt->data) {
				//the new should be a subset of the old so cannot overflow
				uint8_t *t = (uint8_t *)av_malloc(new_pkt.size + AV_INPUT_BUFFER_PADDING_SIZE);
				if (t) {
					memcpy(t, new_pkt.data, new_pkt.size);
					memset(t + new_pkt.size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
					new_pkt.data = t;
					new_pkt.buf = NULL;
					a = 1;
				} else {
					a = AVERROR(ENOMEM);
				}
			}

			if (a > 0) {
				new_pkt.buf = av_buffer_create(new_pkt.data, new_pkt.size, av_buffer_default_free, NULL, 0);
				if (new_pkt.buf) {
					pkt->side_data = NULL;
					pkt->side_data_elems = 0;
					av_packet_unref(pkt);
				} else {
					av_freep(&new_pkt.data);
					a = AVERROR(ENOMEM);
				}
			}

			if (a < 0) {
				av_log(codec, AV_LOG_ERROR, "Failed to open bitstream filter %s for stream %d with codec %s",
					bsfc->filter->name, pkt->stream_index, codec->codec ? codec->codec->name : "copy");
				ret = a;
				break;
			}

			*pkt = new_pkt;

			bsfc = bsfc->next;
		}

		return ret;
	}

	enum AVCodecID get_codec_id(const track &tr) {
#if 0
		const AVCodecTag *tags[2] = { NULL, NULL };

		if (tr.audio.sample_rate != 0)
			tags[0] = avformat_get_mov_audio_tags();
		if (tr.video.width != 0)
			tags[0] = avformat_get_mov_video_tags();
#endif
		switch (tr.media_subtype) {
		case GF_ISOM_SUBTYPE_MPEG4:
#if 0
			tags[0] = ff_mp4_obj_type;
			return av_codec_get_id(tags, tr.esd.dconf.objectTypeIndication);
#else

			switch (tr.esd.dconf.streamType) {
			case GF_STREAM_AUDIO:
				return AV_CODEC_ID_AAC;
			case GF_STREAM_VISUAL:
				return AV_CODEC_ID_MPEG4;
			default:
				break;
			}
#endif
		case GF_ISOM_SUBTYPE_AVC_H264:
		case GF_ISOM_SUBTYPE_AVC2_H264:
		case GF_ISOM_SUBTYPE_AVC3_H264:
		case GF_ISOM_SUBTYPE_AVC4_H264:
			setup_filter("h264_mp4toannexb");
			return AV_CODEC_ID_H264;
		case GF_ISOM_SUBTYPE_HVC1:
		case GF_ISOM_SUBTYPE_HEV1:
		case GF_ISOM_SUBTYPE_HVC2:
		case GF_ISOM_SUBTYPE_HEV2:
		case GF_ISOM_SUBTYPE_HVT1:
		case GF_ISOM_SUBTYPE_SHC1:
		case GF_ISOM_SUBTYPE_SHV1:
			setup_filter("hevc_mp4toannexb");
			return AV_CODEC_ID_HEVC;
		default:
			break;
		}

		return AV_CODEC_ID_NONE;
	}

	void setup_filter(const std::string &name) {
		AVBitStreamFilterContext *filter = av_bitstream_filter_init(name.c_str());
		if (filter) {
			m_filters.push_back(filter);
		}
	}

	void setup_stream(const track &tr, enum AVCodecID codec_id) {
		AVCodecContext *c = m_stream->codec;
		c->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;

		m_stream->id = m_oc->nb_streams - 1;
		c->codec_id = codec_id;

		int extradata_size = tr.esd.dconf.decoderSpecificInfo.data.size();
		if (!c->extradata && !c->extradata_size && extradata_size) {
			uint8_t *extradata = (uint8_t *)av_malloc(extradata_size);
			if (!extradata) {
				elliptics::throw_error(-ENOMEM, "could not allocate extradata, size: %d", extradata_size);
			}

			memcpy(extradata, tr.esd.dconf.decoderSpecificInfo.data.data(), extradata_size);
			c->extradata = extradata;
			c->extradata_size = extradata_size;
		}

		switch (m_codec->type) {
		case AVMEDIA_TYPE_AUDIO:
			c->sample_fmt  = m_codec->sample_fmts ? m_codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
			c->bit_rate = tr.audio.bps * tr.audio.sample_rate;
			c->sample_rate = tr.audio.sample_rate;
			c->channels = tr.audio.channels;
			c->channel_layout = AV_CH_LAYOUT_STEREO;
			m_stream->time_base = (AVRational){ 1, c->sample_rate };
			break;
		case AVMEDIA_TYPE_VIDEO:
			c->width = tr.video.width;
			c->height = tr.video.height;
			// stream timebase will be internally changed to 1.90000 for mpeg2ts,
			// thus we can not use our saved fps num/denum fields, use media timescale instead
			// when it is 24000 it plays with correct speed, without rescaling it is about 3 times faster (90/24)
			m_stream->time_base = (AVRational){ 1, (int)tr.media_timescale };
			c->time_base = m_stream->time_base;

			c->ticks_per_frame = 2;
			c->gop_size = 24;
			c->pix_fmt = AV_PIX_FMT_YUV420P;
			if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
				/* just for testing, we also add B frames */
				c->max_b_frames = 2;
			}
			if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
				/* Needed to avoid using macroblocks in which some coeffs overflow.
				 * This does not happen with normal video, it just happens here as
				 * the motion of the chroma plane does not match the luma plane.
				 */
				c->mb_decision = 2;
			}
			c->sample_aspect_ratio = (AVRational){ (int)tr.video.sar_w, (int)tr.video.sar_h };
			break;
		default:
			break;
		}

		if (m_oc->oformat->flags & AVFMT_GLOBALHEADER)
			c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
};

class mpeg2ts_writer {
public:
	mpeg2ts_writer(const std::string &tmp_dir, const track &tr) : m_track(tr), m_tmp_dir(tmp_dir) {
		int err = avformat_alloc_output_context2(&m_format_context, NULL, "mpegts", NULL);
		if (err < 0) {
			char err_buf[256];
			elliptics::throw_error(err, "could not allocate output context: %s",
					av_make_error_string(err_buf, sizeof(err_buf), err));
		}
	}

	~mpeg2ts_writer() {
		avformat_free_context(m_format_context);
	}

	int create(const writer_options &opt, std::vector<char> &movie_data) {
		int err;
		AVOutputFormat *fmt = m_format_context->oformat;
		char filename[128];
		AVPacket pkt;
		uint64_t duration;
		std::unique_ptr<output_stream> stream;

		snprintf(filename, sizeof(filename), "%s/%ld.%ld.%p.ts",
				m_tmp_dir.c_str(), (unsigned long)opt.dts_start, (unsigned long)opt.dts_start_absolute, opt.sample_data);
		unlink(filename);

		if (!(fmt->flags & AVFMT_NOFILE)) {
			err = avio_open(&m_format_context->pb, filename, AVIO_FLAG_WRITE);
			if (err < 0) {
				char err_buf[256];
				fprintf(stderr, "Could not open '%s': %s\n", filename, av_make_error_string(err_buf, sizeof(err_buf), err));
				goto out_exit;
			}
		}

		try {
			stream.reset(new output_stream(m_track, m_format_context));

			err = stream->open();
			if (err < 0) {
				char err_buf[256];
				fprintf(stderr, "Could not open stream into '%s': %s\n",
						filename, av_make_error_string(err_buf, sizeof(err_buf), err));
				goto out_free_movie;
			}
		} catch (const elliptics::error &e) {
			err = e.error_code();
			fprintf(stderr, "Caught elliptics exception while creating output stream for '%s': %s [%d]\n",
					filename, e.what(), err);
			goto out_free_movie;
		} catch (const std::exception &e) {
			err = -EINVAL;
			fprintf(stderr, "Caught exception while creating output stream for '%s': %s\n", filename, e.what());
			goto out_free_movie;
		}

		err = avformat_write_header(m_format_context, NULL);
		if (err < 0) {
			char err_buf[256];
			fprintf(stderr, "Could not write header into '%s': %s [%d]\n",
					filename, av_make_error_string(err_buf, sizeof(err_buf), err), err);
			goto out_free_movie;
		}


		av_init_packet(&pkt);
		for (ssize_t i = opt.pos_start; i <= opt.pos_end; ++i) {
			const sample &ms = m_track.samples[i];

			if (i < (ssize_t)m_track.samples.size() - 1) {
				duration = m_track.samples[i + 1].dts - ms.dts;
			}

			size_t offset = ms.offset - m_track.samples[opt.pos_start].offset;
			if (offset >= opt.sample_data_size) {
				err = -E2BIG;
				goto check;
			}

			if (ms.length + offset > opt.sample_data_size) {
				err = -EINVAL;
				goto check;
			}

			pkt.stream_index = stream->stream_index();
			pkt.size = ms.length;
			pkt.data = (uint8_t *)opt.sample_data + offset;
			pkt.dts = ms.dts - m_track.samples[opt.pos_start].dts;
			pkt.pts = pkt.dts + ms.cts_offset;
			if (ms.is_rap)
				pkt.flags |= AV_PKT_FLAG_KEY;
			pkt.duration = duration;
			pkt.pos = -1;

			stream->rescale_ts(&pkt);
#if 0
			printf("%zd/%zd, adding sample, length: %d, flags: 0x%x, "
					"dts: %lu, ms.dts: %lu, first.dts: %lu, duration: %lu, pts: %lu, "
					"offset: %zd\n",
					i, opt.pos_end, pkt.size, pkt.flags,
					(unsigned long)pkt.dts,
					(unsigned long)ms.dts, (unsigned long)m_track.samples[opt.pos_start].dts,
					(unsigned long)duration,
					(unsigned long)pkt.pts,
					offset);
#endif

			err = stream->apply_filters(&pkt);
			if (err < 0) {
				char err_buf[256];
				fprintf(stderr, "Could not apply filters for '%s': %s [%d]\n",
						filename, av_make_error_string(err_buf, sizeof(err_buf), err), err);
				goto check;
			}


			err = av_interleaved_write_frame(m_format_context, &pkt);
			if (err < 0) {
				char err_buf[256];
				fprintf(stderr, "Could not write interleaved frame into '%s': %s [%d]\n",
						filename, av_make_error_string(err_buf, sizeof(err_buf), err), err);
				goto check;
			}
check:
			av_packet_unref(&pkt);
			if (err < 0)
				goto out_free_movie;
		}

		av_write_trailer(m_format_context);

out_free_movie:
		if (!(fmt->flags & AVFMT_NOFILE)) {
			avio_closep(&m_format_context->pb);
		}
out_exit:
		if (err >= 0) {
			std::ifstream in(filename, std::ifstream::binary);
			in.seekg(0, in.end);
			movie_data.resize(in.tellg());
			in.seekg(0, in.beg);
			in.read((char *)movie_data.data(), movie_data.size());
		}

		unlink(filename);
		return err;
	}

private:
	track m_track;
	std::string m_tmp_dir;
	AVFormatContext *m_format_context;
};

}} // namespace ioremap::nulla

#endif // __NULLA_MPEG2TS_WRITER_HPP
