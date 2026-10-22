/**
 * @file      ui_ssh.cpp
 * @brief     SSH client app — terminal UI over libssh.
 *
 * On hardware we use ewpa/LibSSH-ESP32 (a port of libssh built on the
 * mbedTLS shipped with Arduino-ESP32). The session runs in its own
 * FreeRTOS task on core 0, with a mutex-protected pair of std::string
 * buffers carrying bytes between the LVGL/UI thread and the SSH task.
 * The emulator (non-ARDUINO) build, and boards without LibSSH in their
 * lib_deps (no USING_LIBSSH — the watches), keep a loopback stub so the UI
 * can be exercised without networking.
 *
 * Layout (480×222 pager): status text + edit (pencil) + connect button on
 * row 1, terminal in the middle, input bar at the bottom. Encoder-click on
 * a textarea toggles edit mode; the same idiom as the text editor.
 *
 * Credentials: host/user/port live in the "ssh" NVS namespace as plaintext
 * (non-secret). The password rides on the same encrypted-secret pipeline as
 * the Telegram bearer (`hal::secret_store/load` under "ssh"/"pw_enc"); when
 * notes_crypto is enabled+unlocked we save it on prompt submit, and the
 * Connect button uses the saved password directly on subsequent runs — no
 * prompts. The pencil (edit) button forces a fresh prompt chain.
 *
 * State reset on onStop (SshApp::onStop):
 *   timers:  poll_timer_                  — lv_timer_del + nullptr
 *   tasks:   backend_->disconnect()       — kills SSH worker task, frees
 *                                            shared_ptr in unique_ptr reset
 *            (LibSshBackend wake_fd_ eventfd — closed in the destructor
 *             after the worker has exited, P7.43)
 *   ui:      ui_hide_back_button()         — releases shared status-bar hook
 *   statics: s_prompt_app                 — cleared if this app owns it
 *            s_live_                      — cleared if this app owns it; a drain
 *                                           posted after exit finds it null
 * Unlike most apps, this one holds state as App members (poll_timer_,
 * backend_) rather than file statics. Add new members alongside and make
 * sure onStop() releases them BEFORE chaining core::App::onStop() — the
 * base destroys the parent panel and any timer pointing into it explodes.
 */
#include "../ui_define.h"
#include "../core/app.h"
#include "../core/app_manager.h"
#include "../core/system.h"
#include "../core/ui_post.h"
#include "../hal/wireless.h"
#include "../hal/secrets.h"
#include "../hal/notes_crypto.h"
#include "app_registry.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef ARDUINO
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#endif
#if defined(ARDUINO) && defined(USING_LIBSSH)
#include <libssh_esp32.h>
#include <libssh/libssh.h>
#include <sys/select.h>
#include <unistd.h>
#include <esp_vfs_eventfd.h>
#endif

// Tracks whether any SSH backend is currently in the Connected state.
// Written by the backend (SSH worker task or loopback stub); read by
// factory.ino loop() as a guard against auto fake-sleep while a terminal
// session is live. volatile is sufficient: bool read/writes are atomic on
// ARM Cortex-M, and the compiler must not optimise away the cross-task read.
static volatile bool s_ssh_session_active = false;

namespace apps {
namespace {

// --- Backend abstraction --------------------------------------------------

struct SshConfig {
    std::string host;
    std::string user;
    std::string password;
    uint16_t    port = 22;
};

class SshBackend {
public:
    enum class State { Idle, Connecting, Connected, Error };

    virtual ~SshBackend() = default;

    // Non-blocking. `connect()` returns immediately; poll state() to advance.
    virtual void  connect(const SshConfig& cfg) = 0;
    virtual void  disconnect() = 0;
    // Raw byte write — caller frames lines with '\n' explicitly. Lets
    // callers send control bytes (Tab, Esc, Ctrl-X, arrow CSI sequences)
    // without having a newline tagged on.
    virtual void  send_bytes(const std::string& bytes) = 0;

    // Drain pending output (raw bytes — caller is responsible for ANSI handling).
    virtual std::string read_pending() = 0;

    virtual State       state() const = 0;
    virtual std::string error_message() const { return ""; }
};

// Loopback backend: fakes a remote shell. Demonstrates the full UI/data path
// without needing networking. Used on boards built without LibSSH (the
// watches) and on the emulator; the pager gets LibSshBackend below.
class LoopbackBackend : public SshBackend {
public:
    void connect(const SshConfig& cfg) override {
        cfg_ = cfg;
        hal::secret_scrub(cfg_.password);  // the stub never authenticates
        state_ = State::Connecting;
        ticks_until_connect_ = 5;  // ~500 ms at 100 ms poll
    }
    void disconnect() override {
        if (state_ == State::Connected) buf_ += "\nConnection closed.\n";
        state_ = State::Idle;
        s_ssh_session_active = false;
    }
    void send_bytes(const std::string& bytes) override {
        if (state_ != State::Connected) return;
        line_buf_ += bytes;
        size_t nl;
        while ((nl = line_buf_.find('\n')) != std::string::npos) {
            std::string line = line_buf_.substr(0, nl);
            line_buf_.erase(0, nl + 1);
            handle_line(line);
        }
    }
    void handle_line(const std::string& line) {
        if (line == "exit" || line == "logout") {
            buf_ += "logout\n";
            state_ = State::Idle;
            s_ssh_session_active = false;
            return;
        }
        if (line == "whoami")        buf_ += cfg_.user + "\n";
        else if (line == "hostname") buf_ += cfg_.host + "\n";
        else if (line == "pwd")      buf_ += "/home/" + cfg_.user + "\n";
        else if (!line.empty())      buf_ += line + ": command not found\n";
        buf_ += prompt();
    }
    std::string read_pending() override {
        if (state_ == State::Connecting && ticks_until_connect_ > 0
            && --ticks_until_connect_ == 0) {
            state_ = State::Connected;
            s_ssh_session_active = true;
            buf_ += "Welcome to LilyGo SSH (loopback stub).\n";
            buf_ += "Type 'exit' to disconnect.\n";
            buf_ += prompt();
        }
        std::string out;
        out.swap(buf_);
        return out;
    }
    State       state() const override { return state_; }
    std::string error_message() const override { return ""; }
private:
    std::string prompt() const {
        return cfg_.user + "@" + cfg_.host + ":~$ ";
    }
    SshConfig   cfg_;
    State       state_ = State::Idle;
    std::string buf_;
    std::string line_buf_;
    int         ticks_until_connect_ = 0;
};

#if defined(ARDUINO) && defined(USING_LIBSSH)
// P7.43: the session task calls this when output lands in an empty
// buffer, so the UI drains it now instead of on the next 100 ms tick.
static void ssh_output_ready();

// libssh-backed implementation. The session is owned by a FreeRTOS task
// pinned to core 0 (the LVGL/UI runs on core 1 in factory.ino). I/O between
// the UI thread and the session task goes through two std::string buffers
// guarded by a single mutex: tiny payloads, single producer/consumer per
// direction, so a mutex is cheaper and simpler than ringbuffers/queues.
//
// libssh handshake (RSA/ECDH + AES-GCM/ChaCha20) needs real headroom — we
// give the task a 32 KB stack. mbedTLS allocates working buffers on the
// heap; on the pager (PSRAM) that's plentiful. The first connect calls
// `libssh_begin()` once via a guarded latch.
class LibSshBackend : public SshBackend {
public:
    LibSshBackend()
        : io_mtx_(xSemaphoreCreateMutex()),
          done_sem_(xSemaphoreCreateBinary()) {}

    ~LibSshBackend() override {
        stop_flag_.store(true);
        wake();
        if (task_) {
            xSemaphoreTake(done_sem_, portMAX_DELAY);
            task_ = nullptr;
        }
        // Only after the worker is gone: it selects on this fd.
        if (wake_fd_ >= 0) close(wake_fd_);
        if (io_mtx_)   vSemaphoreDelete(io_mtx_);
        if (done_sem_) vSemaphoreDelete(done_sem_);
    }

    void connect(const SshConfig& cfg) override {
        State cur = state_.load();
        if (cur != State::Idle && cur != State::Error) return;

        // A previous task already gave done_sem_ on its way out; drain it
        // before spawning a fresh one so the binary semaphore stays in sync.
        if (task_) {
            xSemaphoreTake(done_sem_, portMAX_DELAY);
            task_ = nullptr;
        }

        cfg_ = cfg;
        stop_flag_.store(false);
        {
            xSemaphoreTake(io_mtx_, portMAX_DELAY);
            err_.clear();
            in_buf_.clear();
            out_buf_.clear();
            xSemaphoreGive(io_mtx_);
        }
        state_.store(State::Connecting);

        ensure_libssh_started();
        // Created once per backend on this (UI) thread and closed only in the
        // destructor, so send_bytes()/disconnect() never write to a closed or
        // reused fd. -1 (eventfd unavailable) keeps a 50 ms select timeout.
        if (wake_fd_ < 0 && ensure_eventfd_registered()) wake_fd_ = eventfd(0, 0);

        BaseType_t ok = xTaskCreatePinnedToCore(
            task_trampoline, "ssh_app", 32 * 1024, this, 5, &task_, 0);
        if (ok != pdPASS) {
            task_ = nullptr;
            hal::secret_scrub(cfg_.password);
            set_error("Failed to create SSH task");
            push_output("[!] Failed to spawn SSH task.\n");
            state_.store(State::Error);
        }
    }

    void disconnect() override {
        stop_flag_.store(true);
        wake();
    }

    void send_bytes(const std::string& bytes) override {
        if (state_.load() != State::Connected || bytes.empty()) return;
        xSemaphoreTake(io_mtx_, portMAX_DELAY);
        in_buf_ += bytes;
        xSemaphoreGive(io_mtx_);
        wake();   // P7.43: the session task sends it now, not on its next poll
    }

    std::string read_pending() override {
        std::string out;
        if (xSemaphoreTake(io_mtx_, pdMS_TO_TICKS(20)) == pdTRUE) {
            out.swap(out_buf_);
            xSemaphoreGive(io_mtx_);
        }
        return out;
    }

    State state() const override { return state_.load(); }

    std::string error_message() const override {
        std::string ret;
        if (xSemaphoreTake(io_mtx_, pdMS_TO_TICKS(20)) == pdTRUE) {
            ret = err_;
            xSemaphoreGive(io_mtx_);
        }
        return ret;
    }

private:
    static void task_trampoline(void* arg) {
        auto* self = static_cast<LibSshBackend*>(arg);
        self->run_session();
        // Covers every early error return inside run_session().
        hal::secret_scrub(self->cfg_.password);
        xSemaphoreGive(self->done_sem_);
        vTaskDelete(nullptr);
    }

    void run_session() {
        ssh_session sess      = ssh_new();
        ssh_channel ch        = nullptr;
        bool        connected = false;  // ssh_connect succeeded
        bool        ch_open   = false;  // ssh_channel_open_session succeeded

        // Shared error exit: record the error, echo it to the terminal,
        // release whatever has been set up so far and park in Error.
        auto fail = [&](const std::string& err, const std::string& out) {
            set_error(err);
            push_output(out);
            if (ch) {
                if (ch_open) ssh_channel_close(ch);
                ssh_channel_free(ch);
            }
            if (connected) ssh_disconnect(sess);
            if (sess) ssh_free(sess);
            state_.store(State::Error);
        };

        if (!sess) {
            fail("ssh_new failed", "[!] ssh_new failed.\n");
            return;
        }

        ssh_options_set(sess, SSH_OPTIONS_HOST, cfg_.host.c_str());
        ssh_options_set(sess, SSH_OPTIONS_USER, cfg_.user.c_str());
        long port = cfg_.port;
        ssh_options_set(sess, SSH_OPTIONS_PORT, &port);
        long timeout = 15;
        ssh_options_set(sess, SSH_OPTIONS_TIMEOUT, &timeout);
        int verbosity = SSH_LOG_NOLOG;
        ssh_options_set(sess, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);

        push_output("Connecting...\n");
        if (ssh_connect(sess) != SSH_OK) {
            std::string e = ssh_get_error(sess);
            fail("Connect failed: " + e, "[!] " + e + "\n");
            return;
        }
        connected = true;

        // TODO: real host key verification. For now the user implicitly
        // trusts the network — same posture as a fresh `ssh -o
        // StrictHostKeyChecking=no` session.
        int auth = ssh_userauth_password(sess, nullptr, cfg_.password.c_str());
        hal::secret_scrub(cfg_.password);  // not needed past auth
        if (auth != SSH_AUTH_SUCCESS) {
            std::string e = ssh_get_error(sess);
            fail("Auth failed: " + e, "[!] Auth failed: " + e + "\n");
            return;
        }

        ch = ssh_channel_new(sess);
        if (!ch || ssh_channel_open_session(ch) != SSH_OK) {
            fail("Failed to open shell channel", "[!] Failed to open channel.\n");
            return;
        }
        ch_open = true;

        if (ssh_channel_request_pty(ch) != SSH_OK
            || ssh_channel_change_pty_size(ch, 80, 24) != SSH_OK
            || ssh_channel_request_shell(ch) != SSH_OK) {
            fail("Failed to request shell", "[!] Failed to request shell.\n");
            return;
        }

        state_.store(State::Connected);
        s_ssh_session_active = true;

#ifdef INPUT_PROFILE
        // Profile build only (P7.43; it shipped in production before):
        // periodic watermark print to size the 32 KB stack (P3.9). The
        // high-water mark is a running minimum since task creation, so this
        // captures the auth/handshake peak too, not just the idle read loop.
        uint32_t last_wm_ms = 0;
#endif

        // P7.43: drain everything readable into a 4 KB buffer (heap: > 1 KB
        // goes to PSRAM, P7.48), send queued input, then sleep in select() on
        // the session socket and wake_fd_ until the server sends or the UI
        // queues input / disconnects. Replaces 2 x 256 B reads + a 50 ms poll.
        // Backpressure: stop reading while kOutCap bytes wait for the UI
        // (poll_tick_cb drains every 100 ms), so one tick never feeds the
        // terminal more than ~16 KB; TCP flow control slows the server.
        constexpr size_t kReadBuf = 4096;
        constexpr size_t kOutCap = 16 * 1024;
        std::unique_ptr<char[]> buf(new char[kReadBuf]);
        const int sock = ssh_get_fd(sess);
        while (!stop_flag_.load()) {
#ifdef INPUT_PROFILE
            uint32_t wm_now = millis();
            if (wm_now - last_wm_ms > 3000) {
                Serial.printf("[stackwm] ssh_app free=%u B\n",
                              (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
                last_wm_ms = wm_now;
            }
#endif

            const size_t pending = pending_output_size();
            const size_t room = pending < kOutCap ? kOutCap - pending : 0;
            std::string out;
            bool read_err = false;
            for (int is_stderr = 0; is_stderr < 2 && !read_err; ++is_stderr) {
                while (out.size() < room) {
                    int n = ssh_channel_read_nonblocking(ch, buf.get(), kReadBuf, is_stderr);
                    if (n == SSH_ERROR) {
                        read_err = is_stderr == 0;   // as before: only stdout errors end it
                        break;
                    }
                    if (n <= 0) break;               // nothing left (0) or SSH_EOF
                    out.append(buf.get(), n);
                }
            }
            push_output(out);
            if (read_err) break;

            if (ssh_channel_is_eof(ch)) break;

            std::string in;
            if (xSemaphoreTake(io_mtx_, pdMS_TO_TICKS(20)) == pdTRUE) {
                in.swap(in_buf_);
                xSemaphoreGive(io_mtx_);
            }
            if (!in.empty()) {
                if (ssh_channel_write(ch, in.data(), in.size()) == SSH_ERROR) break;
            }

            // Something moved: look again before sleeping (more may be ready).
            if (!out.empty() || !in.empty()) continue;
            if (room == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));   // the UI hasn't drained yet
                continue;
            }
            wait_io(sock);
        }

        push_output("\nConnection closed.\n");
        ssh_channel_close(ch);
        ssh_channel_free(ch);
        ssh_disconnect(sess);
        ssh_free(sess);
        s_ssh_session_active = false;
        state_.store(State::Idle);
    }

    size_t pending_output_size() {
        size_t n = SIZE_MAX;   // lock busy: treat as full and retry shortly
        if (xSemaphoreTake(io_mtx_, pdMS_TO_TICKS(20)) == pdTRUE) {
            n = out_buf_.size();
            xSemaphoreGive(io_mtx_);
        }
        return n;
    }

    /** Wake the session task out of wait_io (UI thread / destructor). */
    void wake() {
        if (wake_fd_ < 0) return;
        uint64_t one = 1;
        (void)write(wake_fd_, &one, sizeof(one));
    }

    /** Sleep until the socket is readable, wake() was called, or the timeout:
     *  1 s with the wake fd (a safety net; stop and input both wake it),
     *  50 ms without it (the old cadence, but server data still wakes it). */
    void wait_io(int sock) {
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            return;
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(sock, &rd);
        int maxfd = sock;
        if (wake_fd_ >= 0) {
            FD_SET(wake_fd_, &rd);
            if (wake_fd_ > maxfd) maxfd = wake_fd_;
        }
        timeval tv;
        tv.tv_sec = wake_fd_ >= 0 ? 1 : 0;
        tv.tv_usec = wake_fd_ >= 0 ? 0 : 50 * 1000;
        int r = select(maxfd + 1, &rd, nullptr, nullptr, &tv);
        if (r < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));   // select failed: degrade to the old poll
            return;
        }
        if (r > 0 && wake_fd_ >= 0 && FD_ISSET(wake_fd_, &rd)) {
            uint64_t v;
            (void)read(wake_fd_, &v, sizeof(v));   // reset the counter
        }
    }

    static bool ensure_eventfd_registered() {
        static std::atomic<int> st{0};   // 0 = not tried, 1 = ok, -1 = failed
        int cur = st.load();
        if (cur != 0) return cur > 0;
        esp_vfs_eventfd_config_t cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
        esp_err_t err = esp_vfs_eventfd_register(&cfg);
        // INVALID_STATE: someone else already registered the driver.
        bool ok = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
        st.store(ok ? 1 : -1);
        return ok;
    }

    void push_output(const std::string& s) {
        if (s.empty()) return;
        xSemaphoreTake(io_mtx_, portMAX_DELAY);
        const bool was_empty = out_buf_.empty();
        out_buf_ += s;
        xSemaphoreGive(io_mtx_);
        // First bytes since the UI last drained: let it show them now rather
        // than on the next 100 ms tick (P7.43).
        if (was_empty) ssh_output_ready();
    }

    void set_error(const std::string& msg) {
        xSemaphoreTake(io_mtx_, portMAX_DELAY);
        err_ = msg;
        xSemaphoreGive(io_mtx_);
    }

    static void ensure_libssh_started() {
        static std::atomic<bool> started{false};
        bool exp = false;
        if (started.compare_exchange_strong(exp, true)) {
            libssh_begin();
        }
    }

    SshConfig          cfg_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool>  stop_flag_{false};
    SemaphoreHandle_t  io_mtx_   = nullptr;
    SemaphoreHandle_t  done_sem_ = nullptr;
    std::string        in_buf_;
    std::string        out_buf_;
    std::string        err_;
    TaskHandle_t       task_     = nullptr;
    int                wake_fd_  = -1;   // eventfd; see connect()
};
#endif  // ARDUINO && USING_LIBSSH

// Boards without LibSSH (the watches) and the emulator get the loopback stub.
std::unique_ptr<SshBackend> make_default_backend() {
#if defined(ARDUINO) && defined(USING_LIBSSH)
    return std::unique_ptr<SshBackend>(new LibSshBackend());
#else
    return std::unique_ptr<SshBackend>(new LoopbackBackend());
#endif
}

// --- ANSI / control-byte filter ------------------------------------------
// Real shells sprinkle output with CSI (ESC '[' ... final), OSC (ESC ']' ...
// BEL or ST), and standalone ESC sequences for color, cursor, title, etc.
// LVGL's textarea renders those as noise. We strip them with a tiny state
// machine that survives across read chunks (a sequence may straddle a 256-
// byte boundary). We also drop lone '\r' (terminal "go to column 0") and
// other C0 control bytes apart from '\n' and '\t'.
class AnsiFilter {
public:
    std::string feed(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (char c : in) {
            unsigned char b = (unsigned char)c;
            switch (state_) {
                case S_TEXT:
                    if (b == 0x1B) { state_ = S_ESC; }
                    else if (b == '\n' || b == '\t') { out.push_back(c); }
                    else if (b == '\r') { /* drop */ }
                    else if (b >= 0x20) { out.push_back(c); }
                    // other C0 controls dropped
                    break;
                case S_ESC:
                    if (b == '[')      state_ = S_CSI;
                    else if (b == ']') state_ = S_OSC;
                    else               state_ = S_TEXT;  // single-byte ESC seq
                    break;
                case S_CSI:
                    // CSI: parameter/intermediate bytes 0x20-0x3F, final 0x40-0x7E.
                    if (b >= 0x40 && b <= 0x7E) state_ = S_TEXT;
                    break;
                case S_OSC:
                    // OSC ends with BEL (0x07) or ST (ESC '\').
                    if (b == 0x07)      state_ = S_TEXT;
                    else if (b == 0x1B) state_ = S_OSC_ESC;
                    break;
                case S_OSC_ESC:
                    state_ = S_TEXT;  // accept any byte as terminator
                    break;
            }
        }
        return out;
    }
    void reset() { state_ = S_TEXT; }
private:
    enum { S_TEXT, S_ESC, S_CSI, S_OSC, S_OSC_ESC } state_ = S_TEXT;
};

// --- Persistent settings --------------------------------------------------
// host/user/port → plain NVS. password → encrypted slot in same namespace.

constexpr const char* SSH_NS    = "ssh";
constexpr const char* SSH_PW_KEY = "pw_enc";

void cfg_load(SshConfig& out) {
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(SSH_NS, true)) return;
    out.host = p.getString("host", "").c_str();
    out.user = p.getString("user", "").c_str();
    out.port = (uint16_t)p.getUShort("port", 22);
    p.end();
#else
    out.host = "example.com";
    out.user = "user";
    out.port = 22;
#endif
}

void cfg_save(const SshConfig& cfg) {
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(SSH_NS, false)) return;
    p.putString("host", cfg.host.c_str());
    p.putString("user", cfg.user.c_str());
    p.putUShort("port", cfg.port);
    p.end();
#else
    (void)cfg;
#endif
}

// Returns the decrypted password, or empty if the slot is missing or notes
// crypto is locked. Caller is responsible for scrubbing.
// The hal stub returns "" on the emulator.
std::string pw_load() { return hal::secret_load(SSH_NS, SSH_PW_KEY); }

// Returns true on successful save, false (with `*err` populated) when the
// notes session is not unlocked. Passing "" clears the slot unconditionally.
// Hardware-only: the emulator's finish_connect_with_pw skips the save.
// Whitespace is significant in a password, so no trim.
#ifdef ARDUINO
bool pw_save(const char* pw, std::string* err) {
    return hal::secret_store(SSH_NS, SSH_PW_KEY, pw, err, /*trim_ws=*/false);
}
#endif

// Shared look of the rounded status-row and special-key buttons.
void style_pill(lv_obj_t* btn) {
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(btn, 13, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(btn, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(btn, UI_COLOR_FG, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(btn, 2, LV_STATE_FOCUSED);
}

// --- App ------------------------------------------------------------------

class SshApp : public core::App {
public:
    SshApp() : core::App("SSH") {}

    void onStart(lv_obj_t* parent) override;
    void onStop()  override;

    static void drain_live();   // P7.43: posted by ssh_output_ready()

    // Public: driven by the static credential-prompt callback chain and the
    // edit-button click handler.
    void start_connect_flow();           // smart: saved creds → direct connect
    void start_prompt_chain();           // unconditional prompt walk
    bool try_saved_connect();            // saved host/user/password → connect
    void finish_connect_with_pw(const char* pw);
    void connect_with(const SshConfig& cfg);

private:
    void build_ui();
    void build_special_keys_row(lv_obj_t* parent);
    void rebuild_status_line();
    void append_terminal(const std::string& chunk);
    void disconnect();
    void process_and_send_input(const std::string& line);
    void update_ctrl_btn_visual();
    static void poll_tick_cb(lv_timer_t* t);
    static void focus_changed_cb(lv_event_t* e);
    static void connect_btn_cb(lv_event_t* e);
    static void edit_btn_cb(lv_event_t* e);
    static void back_btn_cb(lv_event_t* e);
    static void term_event_cb(lv_event_t* e);
    static void input_event_cb(lv_event_t* e);
    static void input_history_cb(lv_event_t* e);
    static void special_key_cb(lv_event_t* e);

    // Lookup: each special-key button stores one of these as user_data.
    // bytes==nullptr means "Ctrl toggle" (modal); otherwise the literal
    // bytes to ship to the backend on click.
    struct KeyAction {
        SshApp*     app;
        const char* bytes;
    };

    lv_obj_t*  top_bar_    = nullptr;
    lv_obj_t*  status_lbl_ = nullptr;
    lv_obj_t*  conn_btn_   = nullptr;
    lv_obj_t*  conn_icon_  = nullptr;
    lv_obj_t*  edit_btn_   = nullptr;
    lv_obj_t*  term_       = nullptr;
    lv_obj_t*  input_      = nullptr;
    lv_obj_t*  keys_row_   = nullptr;
    lv_obj_t*  ctrl_btn_   = nullptr;
    lv_obj_t*  ctrl_lbl_   = nullptr;
    lv_timer_t* poll_timer_ = nullptr;

    // Byte length of the terminal textarea, tracked incrementally so the hot
    // append path doesn't copy the whole (up to ~8 KB) buffer every chunk just
    // to check whether it needs trimming. See append_terminal().
    size_t     term_len_   = 0;

    std::unique_ptr<SshBackend> backend_;
    SshConfig cfg_;
    AnsiFilter ansi_;
    SshBackend::State last_state_ = SshBackend::State::Idle;
    bool       ctrl_armed_ = false;
    KeyAction  key_actions_[7] = {};

    // Command history. Newest entry is at the back. `history_idx_` points at
    // the entry currently shown in the input bar (-1 = "nothing recalled,
    // input bar holds the user's live draft"). `history_draft_` snapshots
    // that live draft when the user first scrolls into history, so scrolling
    // back past the newest entry restores what they were typing.
    std::vector<std::string> history_;
    int                      history_idx_   = -1;
    std::string              history_draft_;
    static constexpr size_t  kHistoryMax    = 64;
    static SshApp*           s_live_;   // the started SshApp, LVGL thread only

    void history_push(const std::string& line);
    bool history_navigate(int dir);  // -1 older, +1 newer
};

SshApp* SshApp::s_live_ = nullptr;

#if defined(ARDUINO) && defined(USING_LIBSSH)
void SshApp::drain_live() {
    if (s_live_ && s_live_->poll_timer_) poll_tick_cb(s_live_->poll_timer_);
}

static void ssh_output_ready() { core::ui_post(SshApp::drain_live); }
#endif

// Credential prompts run as a chain of static callbacks. We accumulate
// fields into a single static SshConfig and a single SshApp* so each step
// can pass control to the next without juggling user-data structs.
SshApp*   s_prompt_app = nullptr;
SshConfig s_prompt_cfg;

void ssh_pw_cb(const char* pw, void* ud) {
    (void)ud;
    SshApp* app = s_prompt_app;
    s_prompt_app = nullptr;
    if (!app || !pw) return;
    app->finish_connect_with_pw(pw);
}

void ssh_user_cb(const char* user, void* ud) {
    (void)ud;
    if (!s_prompt_app || !user || !*user) { s_prompt_app = nullptr; return; }
    s_prompt_cfg.user = user;
    std::string subtitle = "for " + s_prompt_cfg.user + "@" + s_prompt_cfg.host;
    ui_passphrase_prompt("SSH password", subtitle.c_str(),
                         /*confirm=*/false, ssh_pw_cb, nullptr);
}

void ssh_host_cb(const char* host, void* ud) {
    (void)ud;
    if (!s_prompt_app || !host || !*host) { s_prompt_app = nullptr; return; }
    s_prompt_cfg.host = host;
    std::string initial_user = s_prompt_cfg.user;
    ui_text_prompt("SSH user", "remote username",
                   initial_user.empty() ? nullptr : initial_user.c_str(),
                   ssh_user_cb, nullptr);
}

void SshApp::start_connect_flow() {
    if (!hw_get_wifi_connected()) {
        ui_msg_pop_up("SSH", "WiFi is not connected.");
        return;
    }
    cfg_load(cfg_);
    if (!try_saved_connect()) start_prompt_chain();
}

// Direct reconnect path: host + user saved AND the encrypted password slot
// decrypts cleanly (notes session unlocked). No prompts. Returns false
// (having connected nothing) when any of those is missing.
bool SshApp::try_saved_connect() {
    if (cfg_.host.empty() || cfg_.user.empty()) return false;
    std::string saved_pw = pw_load();
    if (saved_pw.empty()) return false;
    SshConfig cfg = cfg_;
    cfg.password = saved_pw;
    connect_with(cfg);
    hal::secret_scrub(saved_pw);
    hal::secret_scrub(cfg.password);
    return true;
}

void SshApp::start_prompt_chain() {
    rebuild_status_line();
    s_prompt_app = this;
    s_prompt_cfg = cfg_;  // seed with the persisted host/user
    s_prompt_cfg.password.clear();
    ui_text_prompt("SSH host", "hostname or IP",
                   cfg_.host.empty() ? nullptr : cfg_.host.c_str(),
                   ssh_host_cb, nullptr);
}

void SshApp::finish_connect_with_pw(const char* pw) {
    if (!pw) return;
    s_prompt_cfg.password = pw;
    cfg_save(s_prompt_cfg);
#ifdef ARDUINO
    // Save the password encrypted so the next Connect skips prompts. If
    // notes crypto is disabled or locked, secret_store refuses with an
    // err — we surface that as a one-line hint in the terminal so the
    // user knows why the next session will prompt again.
    std::string err;
    bool saved = pw_save(pw, &err);
    if (saved) {
        append_terminal("[i] Password saved (encrypted).\n");
    } else if (!err.empty()) {
        append_terminal("[i] Password not saved: " + err + "\n");
    }
#endif
    connect_with(s_prompt_cfg);
    hal::secret_scrub(s_prompt_cfg.password);
}

void SshApp::connect_with(const SshConfig& cfg) {
    // cfg_ only needs host/user/port; the backend keeps its own copy of
    // the password, so never let it linger in the App.
    cfg_ = cfg;
    hal::secret_scrub(cfg_.password);
    if (!backend_) return;
    ansi_.reset();
    append_terminal("Connecting to " + cfg.host + "...\n");
    backend_->connect(cfg);
    last_state_ = backend_->state();
    rebuild_status_line();
}

void SshApp::onStart(lv_obj_t* parent) {
    setRoot(parent);
    backend_ = make_default_backend();
    cfg_load(cfg_);
    build_ui();

    // 100 ms tick: the loopback stub and state changes; LibSsh output also
    // posts an immediate drain (P7.43).
    poll_timer_ = lv_timer_create(poll_tick_cb, 100, this);
    s_live_ = this;

    if (!hw_get_wifi_connected()) {
        append_terminal("[!] WiFi not connected. Connect via the home screen "
                        "toggle before opening a session.\n\n");
    }
#ifdef ARDUINO
    // Hint when an encrypted password exists but the notes session is
    // locked — tells the user why Connect will still prompt.
    if (hal::secret_exists(SSH_NS, SSH_PW_KEY) && !notes_crypto_is_unlocked()) {
        append_terminal("[i] Saved password is locked. Unlock notes "
                        "encryption to skip the prompt.\n\n");
    }
#endif
    rebuild_status_line();

    // Auto-connect to the last host when everything we need is on hand:
    // WiFi up, saved host+user, and the encrypted password slot decrypts.
    // Anything missing → stay idle, no prompts on app entry.
    if (hw_get_wifi_connected()) try_saved_connect();
}

void SshApp::onStop() {
    if (s_live_ == this) s_live_ = nullptr;
    if (poll_timer_) { lv_timer_del(poll_timer_); poll_timer_ = nullptr; }
    if (backend_)    { backend_->disconnect(); backend_.reset(); }
    ui_hide_back_button();
    if (s_prompt_app == this) s_prompt_app = nullptr;
    core::App::onStop();
}

void SshApp::build_ui() {
    lv_obj_t* parent = getRoot();
    lv_obj_set_style_bg_color(parent, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 0, 0);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 4, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_group_t* grp = lv_group_get_default();

    ui_show_back_button(back_btn_cb);

    // --- Status row: target text + edit + connect/disconnect button -----
    // Auto-collapses to height 0 when neither edit nor connect is focused;
    // see focus_changed_cb(). Children stay in the focus group, just clipped.
    top_bar_ = lv_obj_create(parent);
    lv_obj_remove_style_all(top_bar_);
    lv_obj_set_size(top_bar_, LV_PCT(100), 28);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top_bar_, 6, 0);
    lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_SCROLLABLE);

    status_lbl_ = lv_label_create(top_bar_);
    lv_obj_set_flex_grow(status_lbl_, 1);
    lv_obj_set_style_text_color(status_lbl_, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(status_lbl_, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(status_lbl_, LV_LABEL_LONG_DOT);

    // Edit (pencil) — forces a fresh prompt chain even if a saved password
    // exists. Lets the user switch hosts without going into Settings.
    edit_btn_ = lv_btn_create(top_bar_);
    lv_obj_set_size(edit_btn_, 36, 26);
    style_pill(edit_btn_);
    {
        lv_obj_t* ic = lv_label_create(edit_btn_);
        lv_label_set_text(ic, LV_SYMBOL_EDIT);
        lv_obj_set_style_text_color(ic, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_14, 0);
        lv_obj_center(ic);
    }
    lv_obj_add_event_cb(edit_btn_, edit_btn_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(edit_btn_, focus_changed_cb, LV_EVENT_FOCUSED, this);
    if (grp) lv_group_add_obj(grp, edit_btn_);

    // Connect button — content-sized so "Disconnect" and "Connect" both fit
    // without clipping. Internal horizontal padding gives the label room
    // beyond the radius corners.
    conn_btn_ = lv_btn_create(top_bar_);
    lv_obj_set_size(conn_btn_, LV_SIZE_CONTENT, 26);
    lv_obj_set_style_pad_hor(conn_btn_, 14, 0);
    lv_obj_set_style_pad_ver(conn_btn_, 0, 0);
    style_pill(conn_btn_);
    conn_icon_ = lv_label_create(conn_btn_);
    lv_obj_set_style_text_color(conn_icon_, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(conn_icon_, &lv_font_montserrat_14, 0);
    lv_label_set_text(conn_icon_, "Connect");
    lv_obj_center(conn_icon_);
    lv_obj_add_event_cb(conn_btn_, connect_btn_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(conn_btn_, focus_changed_cb, LV_EVENT_FOCUSED, this);
    if (grp) lv_group_add_obj(grp, conn_btn_);

    // --- Terminal output -------------------------------------------------
    term_ = lv_textarea_create(parent);
    lv_obj_set_width(term_, LV_PCT(100));
    lv_obj_set_flex_grow(term_, 1);
    lv_textarea_set_one_line(term_, false);
    lv_textarea_set_cursor_click_pos(term_, false);
    lv_textarea_set_text(term_, "");
    term_len_ = 0;
    lv_obj_add_flag(term_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(term_, lv_color_hex(0x0a0c10), 0);  // terminal stays dark in every UI style
    lv_obj_set_style_bg_opa(term_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(term_, UI_COLOR_MUTED, 0);
    lv_obj_set_style_border_width(term_, 1, 0);
    lv_obj_set_style_border_opa(term_, LV_OPA_30, 0);
    lv_obj_set_style_text_color(term_, lv_color_hex(0xb8e0a8), 0);
    // Terminal font is user-configurable via Settings » Fonts » SSH.
    // Default is Montserrat 10 (packs more content per line); switching to
    // a mono face (Unscii / Courier / JetBrains Mono) preserves column
    // alignment for ls/top/htop output at the cost of density.
    // Apply on both the textarea (cursor sizing) and the inner label that
    // actually renders the server output — text_font inheritance from the
    // textarea didn't reach the label here, leaving output stuck on the
    // theme default.
    {
        const lv_font_t *ssh_font = get_ssh_font();
        lv_obj_set_style_text_font(term_, ssh_font, 0);
        lv_obj_t *term_lbl = lv_textarea_get_label(term_);
        if (term_lbl) lv_obj_set_style_text_font(term_lbl, ssh_font, 0);
    }
    lv_obj_set_style_pad_all(term_, 3, 0);
    lv_obj_set_style_text_line_space(term_, 1, 0);
    lv_obj_set_style_opa(term_, LV_OPA_TRANSP, LV_PART_CURSOR);
    // Preprocess so we run before the textarea class handler — that lets us
    // (a) toggle edit mode on encoder click without the default '\n' insert,
    // and (b) repurpose arrow keys in edit mode to scroll the buffer instead
    // of moving the (hidden) cursor.
    lv_obj_add_event_cb(term_, term_event_cb,
                        (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS),
                        this);
    lv_obj_add_event_cb(term_, focus_changed_cb, LV_EVENT_FOCUSED, this);
    if (grp) lv_group_add_obj(grp, term_);

    // --- Special-key row (Tab / Esc / Ctrl-toggle / arrows) --------------
    build_special_keys_row(parent);

    // --- Input bar -------------------------------------------------------
    input_ = lv_textarea_create(parent);
    lv_obj_set_size(input_, LV_PCT(100), 28);
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_placeholder_text(input_, "Type command, Enter to send");
    lv_obj_set_style_text_font(input_, &lv_font_unscii_8, 0);
    lv_obj_set_style_bg_color(input_, UI_COLOR_SURFACE, 0);
    lv_obj_set_style_border_color(input_, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(input_, 1, 0);
    lv_obj_set_style_border_opa(input_, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(input_, 4, 0);
    lv_obj_add_event_cb(input_, input_event_cb, LV_EVENT_ALL, this);
    // Preprocess KEY so we beat the textarea class handler — that lets the
    // encoder rotation walk command history instead of moving the cursor
    // when the input is in edit mode.
    lv_obj_add_event_cb(input_, input_history_cb,
                        (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS),
                        this);
    lv_obj_add_event_cb(input_, focus_changed_cb, LV_EVENT_FOCUSED, this);
    if (grp) lv_group_add_obj(grp, input_);

    enable_keyboard();

    if (grp && conn_btn_) lv_group_focus_obj(conn_btn_);
}

void SshApp::rebuild_status_line() {
    if (!status_lbl_ || !backend_) return;
    const char* state_str = "idle";
    lv_color_t  state_col = UI_COLOR_MUTED;
    switch (backend_->state()) {
        case SshBackend::State::Idle:       state_str = "idle";       state_col = UI_COLOR_MUTED; break;
        case SshBackend::State::Connecting: state_str = "connecting"; state_col = lv_palette_main(LV_PALETTE_AMBER); break;
        case SshBackend::State::Connected:  state_str = "connected";  state_col = lv_palette_main(LV_PALETTE_GREEN); break;
        case SshBackend::State::Error:      state_str = "error";      state_col = lv_palette_main(LV_PALETTE_RED); break;
    }
    char buf[160];
    if (cfg_.host.empty()) {
        snprintf(buf, sizeof(buf), "(no target) - %s", state_str);
    } else {
        snprintf(buf, sizeof(buf), "%s@%s:%u - %s",
                 cfg_.user.empty() ? "?" : cfg_.user.c_str(),
                 cfg_.host.c_str(), (unsigned)cfg_.port, state_str);
    }
    lv_label_set_text(status_lbl_, buf);
    lv_obj_set_style_text_color(status_lbl_, state_col, 0);

    if (conn_icon_) {
        bool live = backend_->state() == SshBackend::State::Connected
                 || backend_->state() == SshBackend::State::Connecting;
        lv_label_set_text(conn_icon_, live ? "Disconnect" : "Connect");
    }
}

void SshApp::append_terminal(const std::string& chunk) {
    if (!term_ || chunk.empty()) return;
    std::string clean = ansi_.feed(chunk);
    if (clean.empty()) return;
    lv_textarea_add_text(term_, clean.c_str());
    term_len_ += clean.size();

    // Bound the scrollback: a long-lived session would otherwise grow the
    // textarea's buffer without limit and eventually OOM. Keep the last
    // ~kTermMax bytes, trimmed to a line boundary so we don't cut mid-line.
    // Only pull the (potentially 8 KB) buffer when the tracked length says a
    // trim is actually due — during bursts this path runs ~10x/s.
    //
    // We can't pass a pointer into the textarea's own buffer back into
    // lv_textarea_set_text(): LVGL's set_text_internal() frees the label's
    // current buffer before copying from its `txt` argument unless that
    // argument *is* the current buffer pointer (by identity) — a `raw +
    // offset` slice aliases freed memory by the time the copy runs. So the
    // cut point is found by scanning the live buffer directly (no copy),
    // and only the surviving tail is ever copied into an owned buffer.
    constexpr size_t kTermMax = 8000;
    if (term_len_ > kTermMax) {
        const char* raw = lv_textarea_get_text(term_);
        size_t len = strlen(raw);
        if (len > kTermMax) {
            size_t cut = len - kTermMax;
            const char* nl = static_cast<const char*>(memchr(raw + cut, '\n', len - cut));
            size_t start = nl ? static_cast<size_t>(nl - raw) + 1 : cut;
            if (!nl) {
                // No newline in the discarded span: fall back to a hard cut
                // at `cut`, but don't split a multi-byte UTF-8 sequence.
                while (start < len && (static_cast<unsigned char>(raw[start]) & 0xC0) == 0x80) ++start;
            }
            std::string kept(raw + start);
            lv_textarea_set_text(term_, kept.c_str());
            term_len_ = kept.size();
        } else {
            term_len_ = len;  // resync if the tracked count ever drifted
        }
    }
    lv_obj_scroll_to_y(term_, LV_COORD_MAX, LV_ANIM_OFF);
}

void SshApp::poll_tick_cb(lv_timer_t* t) {
    SshApp* self = (SshApp*)lv_timer_get_user_data(t);
    if (!self || !self->backend_) return;
    std::string chunk = self->backend_->read_pending();
    if (!chunk.empty()) self->append_terminal(chunk);
    // Compare against the *previous* tick's state (the state changes happen
    // on the SSH task, not in this tick), so capturing prev within the tick
    // would never see a transition.
    auto cur = self->backend_->state();
    if (cur != self->last_state_) {
        self->last_state_ = cur;
        self->rebuild_status_line();
    }
}

void SshApp::connect_btn_cb(lv_event_t* e) {
    SshApp* self = (SshApp*)lv_event_get_user_data(e);
    if (!self || !self->backend_) return;
    auto st = self->backend_->state();
    if (st == SshBackend::State::Connected
        || st == SshBackend::State::Connecting) {
        self->disconnect();
    } else {
        self->start_connect_flow();
    }
}

void SshApp::edit_btn_cb(lv_event_t* e) {
    SshApp* self = (SshApp*)lv_event_get_user_data(e);
    if (!self) return;
    if (!hw_get_wifi_connected()) {
        ui_msg_pop_up("SSH", "WiFi is not connected.");
        return;
    }
    cfg_load(self->cfg_);
    self->start_prompt_chain();
}

void SshApp::back_btn_cb(lv_event_t* e) {
    (void)e;
    hw_feedback();
    menu_show_deferred();
}

void SshApp::term_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    lv_obj_t*   obj   = (lv_obj_t*)lv_event_get_target(e);
    lv_group_t* g     = lv_obj_get_group(obj);
    if (!g) return;
    uint32_t    key   = lv_event_get_key(e);
    lv_indev_t* indev = lv_indev_get_act();
    bool is_encoder   = indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER;
    bool editing      = lv_group_get_editing(g);

    // Encoder click toggles edit mode. Stopping the event keeps the textarea
    // class handler from inserting '\n' on the way in.
    if (is_encoder && key == LV_KEY_ENTER) {
        lv_group_set_editing(g, !editing);
        lv_event_stop_processing(e);
        return;
    }

    // While focused and in edit mode, repurpose arrows / wheel rotation to
    // scroll the terminal buffer. The cursor is hidden (LV_OPA_TRANSP), so
    // the default cursor-move behavior is invisible and useless to the user.
    if (editing && lv_group_get_focused(g) == obj) {
        if (ui_scroll_key_clamped(obj, key, 30, LV_ANIM_ON))
            lv_event_stop_processing(e);
    }
}

void SshApp::input_event_cb(lv_event_t* e) {
    SshApp* self = (SshApp*)lv_event_get_user_data(e);
    if (!self || !self->input_) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
    lv_group_t* g = (lv_group_t*)lv_obj_get_group(obj);

    if (code != LV_EVENT_KEY) return;

    uint32_t key = lv_event_get_key(e);
    if (key != LV_KEY_ENTER) return;

    lv_indev_t* indev = lv_indev_get_act();
    bool editing = g && lv_group_get_editing(g);
    bool is_encoder = indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER;

    if (is_encoder) {
        // Encoder click: toggle edit mode; on the exit transition (editing
        // was true), also send the line. Strip the '\n' the textarea
        // inserted on the way in.
        if (editing) {
            lv_textarea_delete_char(obj);
            const char* txt = lv_textarea_get_text(self->input_);
            std::string line = txt ? txt : "";
            lv_textarea_set_text(self->input_, "");
            self->process_and_send_input(line);
            if (g) lv_group_set_editing(g, false);
        } else {
            if (g) lv_group_set_editing(g, true);
        }
        lv_event_stop_processing(e);
        return;
    }

    // Hardware-keyboard ENTER. The textarea inserted '\n' before our handler
    // fires; read the buffer, strip the trailing newline, send. Don't gate
    // on `editing` — keypads insert chars on focus regardless of LVGL's
    // encoder-style edit mode, so requiring `editing` would skip the send
    // path entirely on the pager keyboard.
    const char* txt = lv_textarea_get_text(self->input_);
    std::string line = txt ? txt : "";
    if (!line.empty() && line.back() == '\n') line.pop_back();
    lv_textarea_set_text(self->input_, "");
    self->process_and_send_input(line);
    lv_event_stop_processing(e);
}

void SshApp::disconnect() {
    if (backend_) backend_->disconnect();
    ctrl_armed_ = false;
    update_ctrl_btn_visual();
    rebuild_status_line();
}

// Maps an ASCII char to its C0 control-byte equivalent (^@..^_, ^?). Returns
// -1 if it isn't a valid Ctrl-target. Lowercase letters are folded to upper.
static int ctrl_byte_for(char c) {
    char up = (c >= 'a' && c <= 'z') ? char(c - 32) : c;
    if (up == '?') return 0x7F;
    if (up >= '@' && up <= '_') return up ^ 0x40;
    return -1;
}

void SshApp::process_and_send_input(const std::string& line) {
    bool connected = backend_
        && backend_->state() == SshBackend::State::Connected;

    // Ctrl-armed: send Ctrl-<first char> as a single byte, drop the rest,
    // disarm. Empty input still disarms (cancel).
    if (ctrl_armed_) {
        if (connected && !line.empty()) {
            int cb = ctrl_byte_for(line[0]);
            if (cb >= 0) backend_->send_bytes(std::string(1, (char)cb));
        } else if (!connected && !line.empty()) {
            append_terminal("[not connected]\n");
        }
        ctrl_armed_ = false;
        update_ctrl_btn_visual();
        return;
    }

    if (!connected) {
        if (!line.empty()) append_terminal("[not connected]\n");
        return;
    }

    // Caret-escape: input of the form "^X" is sent as a single control byte
    // with no trailing newline. Anything longer is treated as plain text.
    if (line.size() == 2 && line[0] == '^') {
        int cb = ctrl_byte_for(line[1]);
        if (cb >= 0) {
            backend_->send_bytes(std::string(1, (char)cb));
            return;
        }
    }

    append_terminal(line + "\n");
    backend_->send_bytes(line + "\n");
    history_push(line);
}

void SshApp::history_push(const std::string& line) {
    if (line.empty()) return;
    // Drop consecutive duplicate (bash-style: HISTCONTROL=ignoredups).
    if (!history_.empty() && history_.back() == line) {
        // still reset cursor below
    } else {
        history_.push_back(line);
        if (history_.size() > kHistoryMax) {
            history_.erase(history_.begin(),
                           history_.begin() + (history_.size() - kHistoryMax));
        }
    }
    history_idx_ = -1;
    history_draft_.clear();
}

bool SshApp::history_navigate(int dir) {
    if (!input_ || history_.empty()) return false;
    int n = (int)history_.size();
    int next;
    if (history_idx_ == -1) {
        if (dir < 0) {
            // Stash the live draft so scrolling back past newest restores it.
            const char* cur = lv_textarea_get_text(input_);
            history_draft_ = cur ? cur : "";
            next = n - 1;
        } else {
            return false;  // already at the live draft, can't go newer
        }
    } else {
        next = history_idx_ + (dir < 0 ? -1 : 1);
        if (next < 0) next = 0;             // clamp at oldest
        if (next >= n) next = -1;           // past newest -> live draft
    }
    history_idx_ = next;
    const std::string& shown = (next == -1) ? history_draft_ : history_[next];
    lv_textarea_set_text(input_, shown.c_str());
    // Park cursor at the end so further typing appends.
    lv_textarea_set_cursor_pos(input_, LV_TEXTAREA_CURSOR_LAST);
    return true;
}

void SshApp::input_history_cb(lv_event_t* e) {
    SshApp* self = (SshApp*)lv_event_get_user_data(e);
    if (!self || !self->input_) return;
    lv_obj_t*   obj = (lv_obj_t*)lv_event_get_target(e);
    lv_group_t* g   = lv_obj_get_group(obj);
    if (!g || !lv_group_get_editing(g)) return;
    lv_indev_t* indev = lv_indev_get_act();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) return;

    uint32_t key = lv_event_get_key(e);
    int dir = 0;
    if (key == LV_KEY_LEFT || key == LV_KEY_UP)        dir = -1;
    else if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN) dir = +1;
    else return;

    self->history_navigate(dir);
    lv_event_stop_processing(e);
}

void SshApp::focus_changed_cb(lv_event_t* e) {
    SshApp* self = (SshApp*)lv_event_get_user_data(e);
    if (!self) return;
    // Only FOCUSED is hooked, on every focusable widget. Target tells us
    // exactly where focus landed; no need to chase DEFOCUSED ordering.
    lv_obj_t* target = (lv_obj_t*)lv_event_get_target(e);
    if (self->top_bar_) {
        bool in_bar = (target == self->conn_btn_ || target == self->edit_btn_);
        lv_obj_set_height(self->top_bar_, in_bar ? 28 : 0);
    }
    if (self->keys_row_) {
        bool in_keys = (lv_obj_get_parent(target) == self->keys_row_);
        lv_obj_set_height(self->keys_row_, in_keys ? 26 : 0);
    }
}

void SshApp::update_ctrl_btn_visual() {
    if (!ctrl_btn_) return;
    if (ctrl_armed_) {
        lv_obj_set_style_bg_color(ctrl_btn_, UI_COLOR_ACCENT, 0);
        if (ctrl_lbl_) lv_obj_set_style_text_color(ctrl_lbl_, UI_COLOR_SURFACE, 0);
    } else {
        lv_obj_set_style_bg_color(ctrl_btn_, UI_COLOR_SURFACE, 0);
        if (ctrl_lbl_) lv_obj_set_style_text_color(ctrl_lbl_, UI_COLOR_ACCENT, 0);
    }
}

void SshApp::special_key_cb(lv_event_t* e) {
    auto* ka = (KeyAction*)lv_event_get_user_data(e);
    if (!ka || !ka->app) return;
    SshApp* self = ka->app;

    // Ctrl button: toggle armed state (no backend round-trip).
    if (!ka->bytes) {
        self->ctrl_armed_ = !self->ctrl_armed_;
        self->update_ctrl_btn_visual();
        return;
    }

    // All other keys: must be connected to ship bytes.
    if (!self->backend_
        || self->backend_->state() != SshBackend::State::Connected) {
        self->append_terminal("[not connected]\n");
        return;
    }
    self->backend_->send_bytes(ka->bytes);
}

void SshApp::build_special_keys_row(lv_obj_t* parent) {
    lv_group_t* grp = lv_group_get_default();

    lv_obj_t* row = lv_obj_create(parent);
    keys_row_ = row;
    lv_obj_remove_style_all(row);
    // Auto-collapses to height 0 when no special-key button is focused;
    // see focus_changed_cb(). Children stay in the focus group, just clipped.
    lv_obj_set_size(row, LV_PCT(100), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    // bytes==nullptr is the modal Ctrl toggle. The arrow keys send the
    // standard xterm CSI sequences; most TUIs (bash readline, vim, less)
    // accept these.
    struct Def { const char* label; const char* bytes; };
    static const Def defs[7] = {
        {"Tab",            "\t"},
        {"Esc",            "\x1b"},
        {"Ctrl",           nullptr},
        {LV_SYMBOL_UP,     "\x1b[A"},
        {LV_SYMBOL_DOWN,   "\x1b[B"},
        {LV_SYMBOL_LEFT,   "\x1b[D"},
        {LV_SYMBOL_RIGHT,  "\x1b[C"},
    };

    for (int i = 0; i < 7; i++) {
        lv_obj_t* b = lv_btn_create(row);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 26);
        style_pill(b);
        lv_obj_set_style_pad_hor(b, 2, 0);
        lv_obj_set_style_pad_ver(b, 0, 0);

        lv_obj_t* lbl = lv_label_create(b);
        lv_label_set_text(lbl, defs[i].label);
        lv_obj_set_style_text_color(lbl, UI_COLOR_ACCENT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_center(lbl);

        key_actions_[i] = KeyAction{this, defs[i].bytes};
        lv_obj_add_event_cb(b, special_key_cb, LV_EVENT_CLICKED, &key_actions_[i]);
        lv_obj_add_event_cb(b, focus_changed_cb, LV_EVENT_FOCUSED, this);
        if (grp) lv_group_add_obj(grp, b);

        if (defs[i].bytes == nullptr) {
            ctrl_btn_ = b;
            ctrl_lbl_ = lbl;
        }
    }
}

}  // namespace

APP_FACTORY(make_ssh_app, SshApp)

}  // namespace apps

// Declared in core/system_hooks.h. Reads the file-scoped volatile flag updated
// by the SSH backends. Safe to call from any task (factory.ino loop()).
bool ssh_session_is_active() { return s_ssh_session_active; }
