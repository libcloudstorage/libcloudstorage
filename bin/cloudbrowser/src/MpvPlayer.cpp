/*****************************************************************************
 * MpvPlayer.cpp
 *
 *****************************************************************************
 * Copyright (C) 2018 VideoLAN
 *
 * Authors: Paweł Wegner <pawel.wegner95@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#include "MpvPlayer.h"

#ifdef WITH_MPV

#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QQuickWindow>
#include <QSGRenderNode>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include "Utility/Utility.h"

const int DURATION_ID = 1;
const int POSITION_ID = 2;
const int IDLE_ID = 3;

namespace {
void on_mpv_events(void *d) {
  emit reinterpret_cast<MpvPlayer *>(d)->onEventOccurred();
}

void on_mpv_redraw(void *ctx) {
  emit reinterpret_cast<MpvPlayer *>(ctx)->onUpdate();
}

static void *get_proc_address_mpv(void *, const char *name) {
  QOpenGLContext *glctx = QOpenGLContext::currentContext();
  if (!glctx) return nullptr;

  return reinterpret_cast<void *>(glctx->getProcAddress(QByteArray(name)));
}

}  // namespace

class MpvRenderer : public QQuickFramebufferObject::Renderer {
 public:
  MpvRenderer(MpvPlayer *player) : player_(player) {
    mpv_set_wakeup_callback(player->mpv_, on_mpv_events, player);
  }

  void render() override {
    mpv_opengl_fbo mpfbo;
    mpfbo.fbo = framebufferObject()->handle();
    mpfbo.w = framebufferObject()->width();
    mpfbo.h = framebufferObject()->height();
    mpfbo.internal_format = 0;

    mpv_render_param params[] = {{MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};
    mpv_render_context_render(player_->mpv_gl_, params);
  }

  QOpenGLFramebufferObject *createFramebufferObject(
      const QSize &size) override {
    initialize();
    return new QOpenGLFramebufferObject(size);
  }

  void initialize() {
    if (!player_->mpv_gl_) {
      mpv_opengl_init_params gl_init_params{get_proc_address_mpv, nullptr,
                                            nullptr};
      mpv_render_param params[]{
          {MPV_RENDER_PARAM_API_TYPE,
           const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
          {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
          {MPV_RENDER_PARAM_INVALID, nullptr}};

      if (mpv_render_context_create(&player_->mpv_gl_, player_->mpv_, params) <
          0)
        throw std::runtime_error("failed to initialize mpv GL context");
      mpv_render_context_set_update_callback(player_->mpv_gl_, on_mpv_redraw,
                                             player_);
      emit player_->onInitialized();
    }
  }

 private:
  MpvPlayer *player_;
  QRectF rect_;
};

MpvPlayer::MpvPlayer(QQuickItem *parent)
    : QQuickFramebufferObject(parent), mpv_(mpv_create()), mpv_gl_(nullptr) {
  if (!mpv_) throw std::runtime_error("could not create mpv context");

  mpv_set_option_string(mpv_, "video-timing-offset", "0");
  mpv_observe_property(mpv_, POSITION_ID, "time-pos", MPV_FORMAT_INT64);
  mpv_observe_property(mpv_, IDLE_ID, "core-idle", MPV_FORMAT_FLAG);
  mpv_request_log_messages(mpv_, "warn");

  if (mpv_initialize(mpv_) < 0)
    throw std::runtime_error("could not initialize mpv context");

  mpv_set_option_string(mpv_, "hwdec", "auto");

  connect(this, &MpvPlayer::onUpdate, this, [=] { update(); },
          Qt::QueuedConnection);
  connect(this, &MpvPlayer::onInitialized, this,
          [=] {
            initialized_ = true;
            if (invoke_load_) executeLoadFile();
          },
          Qt::QueuedConnection);
  connect(this, &MpvPlayer::onEventOccurred, this, &MpvPlayer::eventOccurred,
          Qt::QueuedConnection);
}

MpvPlayer::~MpvPlayer() {
  if (mpv_gl_) {
    mpv_render_context_free(mpv_gl_);
  }
  mpv_terminate_destroy(mpv_);
}

QString MpvPlayer::uri() const { return uri_; }

void MpvPlayer::setUri(QString uri) {
  uri_ = uri;
  emit uriChanged();
  if (!initialized_) {
    invoke_load_ = true;
  } else {
    executeLoadFile();
  }
}

qreal MpvPlayer::position() const { return position_; }

void MpvPlayer::setPosition(qreal position) {
  position_ = position;
  emit positionChanged();
  int64_t p = position * duration() / 1000;
  mpv_set_property_async(mpv_, POSITION_ID, "playback-time", MPV_FORMAT_INT64,
                         &p);
}

int MpvPlayer::volume() const { return volume_; }

void MpvPlayer::setVolume(int v) {
  volume_ = v;
  emit volumeChanged();
  int64_t volume = v;
  mpv_set_property_async(mpv_, 0, "ao-volume", MPV_FORMAT_INT64, &volume);
}

int MpvPlayer::duration() const { return duration_; }

bool MpvPlayer::ended() const { return ended_; }

bool MpvPlayer::playing() const { return playing_; }

void MpvPlayer::play() {
  int flag = false;
  mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvPlayer::pause() {
  int flag = true;
  mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &flag);
}

QQuickFramebufferObject::Renderer *MpvPlayer::createRenderer() const {
  return new MpvRenderer(const_cast<MpvPlayer *>(this));
}

void MpvPlayer::eventOccurred() {
  while (auto event = mpv_wait_event(mpv_, 0)) {
    if (event->event_id == MPV_EVENT_GET_PROPERTY_REPLY) {
      auto property = reinterpret_cast<mpv_event_property *>(event->data);
      if (event->reply_userdata == DURATION_ID) {
        duration_ = 1000 * *reinterpret_cast<int64_t *>(property->data);
        emit durationChanged();
      }
    } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
      mpv_get_property_async(mpv_, DURATION_ID, "duration", MPV_FORMAT_INT64);
    } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
      auto property = reinterpret_cast<mpv_event_property *>(event->data);
      if (event->reply_userdata == POSITION_ID && property->data) {
        position_ =
            1000.0 * *reinterpret_cast<int64_t *>(property->data) / duration();
        emit positionChanged();
      } else if (event->reply_userdata == IDLE_ID) {
        playing_ = !*reinterpret_cast<int *>(property->data);
        emit playingChanged();
      }
    } else if (event->event_id == MPV_EVENT_END_FILE) {
      auto e = reinterpret_cast<mpv_event_end_file *>(event->data);
      if (e->reason == MPV_END_FILE_REASON_EOF ||
          e->reason == MPV_END_FILE_REASON_ERROR) {
        ended_ = true;
        emit endedChanged();
      }
    } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
      auto e = reinterpret_cast<mpv_event_log_message *>(event->data);
      cloudstorage::util::log(e->text);
    } else if (event->event_id == MPV_EVENT_NONE) {
      break;
    }
  }
}

void MpvPlayer::executeLoadFile() {
  mpv_command_string(mpv_, ("loadfile \"" + uri_.toStdString() + "\"").c_str());
  ended_ = false;
}

#endif  // WITH_MPV
