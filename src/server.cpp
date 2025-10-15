// server.cpp
// Compile: g++ -std=c++17 server.cpp -o server -lmicrohttpd -pthread
// Run: ./server
//
// Serves files from MOVIE_DIR using libmicrohttpd. Supports Range requests.
// Prints live throughput (MB/s) and an estimated fps (based on provided
// bitrate/fps query params).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>
#include <microhttpd.h>
#include <string>
#include <sys/stat.h>

#define PORT 8000
#define MOVIE_DIR "/home/lucca/Movies"
#define READ_CHUNK (64 * 1024)

struct FileState {
  FILE *file = nullptr;
  uint64_t offset_base = 0; // starting byte in file to serve (for Range)
  uint64_t file_size = 0;   // total file size
  std::atomic<uint64_t> total_sent{
      0}; // bytes sent (client-facing, uncompressed)
  std::atomic<uint64_t> since_last{0}; // bytes since last print
  std::chrono::steady_clock::time_point last_print =
      std::chrono::steady_clock::now();
  uint64_t estimated_bitrate =
      8000000;              // bits/sec default (used to estimate fps)
  unsigned target_fps = 60; // fps used for estimation
};

static ssize_t file_reader(void *cls, uint64_t pos, char *buf, size_t max) {
  FileState *s = reinterpret_cast<FileState *>(cls);
  // Seek to the requested position relative to the chosen offset base
  if (fseeko(s->file, (off_t)(s->offset_base + pos), SEEK_SET) != 0) {
    return MHD_CONTENT_READER_END_OF_STREAM;
  }
  size_t r = fread(buf, 1, max, s->file);
  if (r == 0) {
    // EOF or error
    if (feof(s->file))
      return MHD_CONTENT_READER_END_OF_STREAM;
    return MHD_CONTENT_READER_END_OF_STREAM;
  }

  s->total_sent += r;
  s->since_last += r;

  // Print stats ~ twice a second
  auto now = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(now - s->last_print).count();
  if (elapsed >= 0.45) {
    uint64_t b = s->since_last.exchange(0);
    double mbps = (double)b / (1024.0 * 1024.0) / elapsed;
    double bytes_per_frame =
        (double)s->estimated_bitrate / (double)s->target_fps / 8.0;
    double frames_per_sec = 0.0;
    if (bytes_per_frame > 0.0) {
      double bytes_per_s = (double)b / elapsed;
      frames_per_sec = bytes_per_s / bytes_per_frame;
    }
    printf("\r📤 %.2f MB/s  |  ~%.1f fps (est)  sent total: %.2f MB ", mbps,
           frames_per_sec, (double)s->total_sent.load() / (1024.0 * 1024.0));
    fflush(stdout);
    s->last_print = now;
  }

  return (ssize_t)r;
}

static void file_free(void *cls) {
  FileState *s = reinterpret_cast<FileState *>(cls);
  if (!s)
    return;
  if (s->file)
    fclose(s->file);
  printf("\n✅ Done. Total sent: %.2f MB\n",
         (double)s->total_sent.load() / (1024.0 * 1024.0));
  fflush(stdout);
  delete s;
}

static bool contains_dotdot(const char *p) {
  if (!p)
    return false;
  const char *q = strstr(p, "..");
  return q != nullptr;
}

static MHD_Result handle_request(void *cls, struct MHD_Connection *connection,
                                 const char *url, const char *method,
                                 const char *version, const char *upload_data,
                                 size_t *upload_data_size, void **con_cls) {
  (void)cls;
  (void)version;
  (void)upload_data;
  (void)upload_data_size;

  if (strcmp(method, "GET") != 0)
    return MHD_NO;

  // Protect against path traversal
  if (contains_dotdot(url))
    return MHD_NO;

  // Build file path
  std::string path = std::string(MOVIE_DIR) + url;

  struct stat st;
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    return MHD_NO;
  uint64_t file_size = (uint64_t)st.st_size;

  // Open file
  FILE *f = fopen(path.c_str(), "rb");
  if (!f)
    return MHD_NO;

  // Query params to help estimation (optional): ?bitrate=8000000&fps=60
  const char *brs =
      MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "bitrate");
  uint64_t bitrate = brs ? (uint64_t)strtoull(brs, nullptr, 10) : 8000000ULL;
  const char *fps_arg =
      MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "fps");
  unsigned fps = fps_arg ? (unsigned)atoi(fps_arg) : 60u;

  // Range header parsing: "Range: bytes=start-end"
  const char *range_hdr =
      MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Range");
  uint64_t start = 0;
  uint64_t end = file_size > 0 ? file_size - 1 : 0;
  bool is_partial = false;

  if (range_hdr && strncmp(range_hdr, "bytes=", 6) == 0) {
    const char *r = range_hdr + 6;
    char *dash = (char *)strchr(r, '-');
    if (dash) {
      // parse start
      if (r != dash) {
        start = (uint64_t)strtoull(r, nullptr, 10);
      } else {
        // suffix syntax "-N" requests last N bytes
        uint64_t suffix = (uint64_t)strtoull(dash + 1, nullptr, 10);
        if (suffix >= file_size)
          start = 0;
        else
          start = file_size - suffix;
      }
      // parse end (optional)
      if (*(dash + 1) != '\0') {
        end = (uint64_t)strtoull(dash + 1, nullptr, 10);
      } else {
        end = file_size - 1;
      }
      if (start > end || start >= file_size) {
        fclose(f);
        return MHD_NO;
      }
      is_partial = true;
    }
  }

  uint64_t content_length = end - start + 1;

  // Allocate state
  FileState *stt = new FileState();
  stt->file = f;
  stt->offset_base = start;
  stt->file_size = file_size;
  stt->estimated_bitrate = bitrate;
  stt->target_fps = fps;

  // Create response from callback with known content_length
  struct MHD_Response *response = MHD_create_response_from_callback(
      (uint64_t)content_length, READ_CHUNK, &file_reader, stt, &file_free);

  if (!response) {
    fclose(f);
    delete stt;
    return MHD_NO;
  }

  // Headers
  // Determine MIME type
  const char *mime = "application/octet-stream";
  if (strstr(url, ".mp4"))
    mime = "video/mp4";
  else if (strstr(url, ".m3u8"))
    mime = "application/x-mpegURL";
  else if (strstr(url, ".ts"))
    mime = "video/mp2t";
  else if (strstr(url, ".html"))
    mime = "text/html; charset=utf-8";
  else if (strstr(url, ".js"))
    mime = "application/javascript";
  else if (strstr(url, ".css"))
    mime = "text/css";
  else if (strstr(url, ".jpg") || strstr(url, ".jpeg"))
    mime = "image/jpeg";
  else if (strstr(url, ".png"))
    mime = "image/png";
  else if (strstr(url, ".gif"))
    mime = "image/gif";
  else if (strstr(url, ".vtt"))
    mime = "text/vtt; charset=utf-8"; // important for subtitles
  else if (strstr(url, ".srt"))
    mime = "application/x-subrip";

  MHD_add_response_header(response, "Content-Type", mime);
  MHD_add_response_header(response, "Accept-Ranges", "bytes");
  // Allow cross-origin requests from LAN devices (helps subtitles & video load
  // on other devices)
  MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");

  int status = MHD_HTTP_OK;
  if (is_partial) {
    char content_range[256];
    snprintf(content_range, sizeof(content_range),
             "bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64, start, end, file_size);
    MHD_add_response_header(response, "Content-Range", content_range);
    status = MHD_HTTP_PARTIAL_CONTENT;
  }

  MHD_Result ret = MHD_queue_response(connection, status, response);
  MHD_destroy_response(response);
  return ret;
}

int main() {
  struct MHD_Daemon *daemon =
      MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, PORT, NULL, NULL,
                       &handle_request, NULL, MHD_OPTION_END);

  if (!daemon) {
    fprintf(stderr, "Failed to start HTTP server.\n");
    return 1;
  }

  printf("✅ Server running at: http://localhost:%d\n", PORT);
  printf("📁 Serving files from: %s\n", MOVIE_DIR);
  printf("Optional query params for estimation: ?bitrate=8000000&fps=60\n");
  printf("Press Enter to stop...\n");
  getchar();

  MHD_stop_daemon(daemon);
  printf("\n🛑 Server stopped.\n");
  return 0;
}
