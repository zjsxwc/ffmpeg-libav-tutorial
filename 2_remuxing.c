// based on https://ffmpeg.org/doxygen/trunk/remuxing_8c-example.html
// and https://ffmpeg.org/doxygen/trunk/avio_reading_8c-example.html#_a12
#include <libavutil/timestamp.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/file.h>

struct buffer_data {
  uint8_t *ptr;
  size_t size; ///< size left in the buffer
};

static int read_packet(void *opaque, uint8_t *buf, int buf_size)
{
  struct buffer_data *bd = (struct buffer_data *)opaque;
  buf_size = FFMIN(buf_size, bd->size);
  if (!buf_size)
    return AVERROR_EOF;
  printf("ptr:%p size:%zu\n", bd->ptr, bd->size);
  /* copy internal buffer data to buf */
  memcpy(buf, bd->ptr, buf_size);
  bd->ptr  += buf_size;
  bd->size -= buf_size;
  return buf_size;
}

int main(int argc, char **argv)
{
  AVFormatContext *input_format_context = NULL, *output_format_context = NULL;
  AVPacket packet;
  const char *in_filename, *out_filename;
  int ret, i;
  int stream_index = 0;
  int *streams_list = NULL;
  int number_of_streams = 0;
  int fragmented_mp4_options = 0;

  AVIOContext *avio_ctx = NULL;
  uint8_t *buffer = NULL, *avio_ctx_buffer = NULL;
  size_t buffer_size, avio_ctx_buffer_size = 4096;
  struct buffer_data bd = { 0 };

  if (argc < 3) {
    printf("You need to pass at least two parameters.\n");
    return -1;
  } else if (argc == 4) {
    fragmented_mp4_options = 1;
  }

  in_filename  = argv[1];
  out_filename = argv[2];

  /* slurp file content into buffer */
  ret = av_file_map(in_filename, &buffer, &buffer_size, 0, NULL);
  if (ret < 0)
    goto end;
  /* fill opaque structure used by the AVIOContext read callback */
  bd.ptr  = buffer;
  bd.size = buffer_size;

  if (!(input_format_context = avformat_alloc_context())) {
    ret = AVERROR(ENOMEM);
    goto end;
  }
  avio_ctx_buffer = av_malloc(avio_ctx_buffer_size);
  if (!avio_ctx_buffer) {
    ret = AVERROR(ENOMEM);
    goto end;
  }
  avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size,
      0, &bd, &read_packet, NULL, NULL);
  if (!avio_ctx) {
    ret = AVERROR(ENOMEM);
    goto end;
  }
  input_format_context->pb = avio_ctx;
  ret = avformat_open_input(&input_format_context, NULL, NULL, NULL);
  if (ret < 0) {
    fprintf(stderr, "Could not open input\n");
    goto end;
  }

  if ((ret = avformat_find_stream_info(input_format_context, NULL)) < 0) {
    fprintf(stderr, "Failed to retrieve input stream information");
    goto end;
  }

  avformat_alloc_output_context2(&output_format_context, NULL, NULL, out_filename);
  if (!output_format_context) {
    fprintf(stderr, "Could not create output context\n");
    ret = AVERROR_UNKNOWN;
    goto end;
  }

  number_of_streams = input_format_context->nb_streams;
  streams_list = av_mallocz_array(number_of_streams, sizeof(*streams_list));

  if (!streams_list) {
    ret = AVERROR(ENOMEM);
    goto end;
  }

  for (i = 0; i < input_format_context->nb_streams; i++) {
    AVStream *out_stream;
    AVStream *in_stream = input_format_context->streams[i];
    AVCodecParameters *in_codecpar = in_stream->codecpar;
    if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
        in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
        in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
      streams_list[i] = -1;
      continue;
    }
    streams_list[i] = stream_index++;
    out_stream = avformat_new_stream(output_format_context, NULL);
    if (!out_stream) {
      fprintf(stderr, "Failed allocating output stream\n");
      ret = AVERROR_UNKNOWN;
      goto end;
    }

    ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
    if (ret < 0) {
      fprintf(stderr, "Failed to copy codec parameters\n");
      goto end;
    }

  }
  // https://ffmpeg.org/doxygen/trunk/group__lavf__misc.html#gae2645941f2dc779c307eb6314fd39f10
  av_dump_format(output_format_context, 0, out_filename, 1);

  // unless it's a no file (we'll talk later about that) write to the disk (FLAG_WRITE)
  // but basically it's a way to save the file to a buffer so you can store it
  // wherever you want.
  if (!(output_format_context->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open(&output_format_context->pb, out_filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
      fprintf(stderr, "Could not open output file '%s'", out_filename);
      goto end;
    }
  }
  AVDictionary* opts = NULL;

  if (fragmented_mp4_options) {
    // https://developer.mozilla.org/en-US/docs/Web/API/Media_Source_Extensions_API/Transcoding_assets_for_MSE
    av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
  }
  // https://ffmpeg.org/doxygen/trunk/group__lavf__encoding.html#ga18b7b10bb5b94c4842de18166bc677cb
  ret = avformat_write_header(output_format_context, &opts);
  if (ret < 0) {
    fprintf(stderr, "Error occurred when opening output file\n");
    goto end;
  }
  while (1) {
    AVStream *in_stream, *out_stream;
    ret = av_read_frame(input_format_context, &packet);
    if (ret < 0)
      break;
    in_stream  = input_format_context->streams[packet.stream_index];
    if (packet.stream_index >= number_of_streams || streams_list[packet.stream_index] < 0) {
      av_packet_unref(&packet);
      continue;
    }
    packet.stream_index = streams_list[packet.stream_index];
    out_stream = output_format_context->streams[packet.stream_index];
    /* copy packet */
    packet.pts = av_rescale_q_rnd(packet.pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
    packet.dts = av_rescale_q_rnd(packet.dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
    packet.duration = av_rescale_q(packet.duration, in_stream->time_base, out_stream->time_base);
    // https://ffmpeg.org/doxygen/trunk/structAVPacket.html#ab5793d8195cf4789dfb3913b7a693903
    packet.pos = -1;

    //https://ffmpeg.org/doxygen/trunk/group__lavf__encoding.html#ga37352ed2c63493c38219d935e71db6c1
    ret = av_interleaved_write_frame(output_format_context, &packet);
    if (ret < 0) {
      fprintf(stderr, "Error muxing packet\n");
      break;
    }
    av_packet_unref(&packet);
  }
  //https://ffmpeg.org/doxygen/trunk/group__lavf__encoding.html#ga7f14007e7dc8f481f054b21614dfec13
  av_write_trailer(output_format_context);
end:
  avformat_close_input(&input_format_context);

  if (avio_ctx) {
    av_freep(&avio_ctx->buffer);
    av_freep(&avio_ctx);
  }
  av_file_unmap(buffer, buffer_size);
  /* close output */
  if (output_format_context && !(output_format_context->oformat->flags & AVFMT_NOFILE))
    avio_closep(&output_format_context->pb);
  avformat_free_context(output_format_context);
  av_freep(&streams_list);
  if (ret < 0 && ret != AVERROR_EOF) {
    fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
    return 1;
  }
  return 0;
}

