#include "viewerwidget.h"

#include "panels/panels.h"
#include "panels/viewer.h"
#include "panels/timeline.h"
#include "project/sequence.h"
#include "project/effect.h"
#include "effects/transition.h"
#include "playback/playback.h"
#include "playback/audio.h"
#include "io/media.h"
#include "ui_timeline.h"

#include <QDebug>
#include <QPainter>
#include <QtMath>

extern "C" {
	#include <libavformat/avformat.h>
}

ViewerWidget::ViewerWidget(QWidget *parent) : QOpenGLWidget(parent)
{	
    multithreaded = true;
    enable_paint = true;
    force_audio = false;
    flip = false;

	QSurfaceFormat format;
	format.setDepthBufferSize(24);
	setFormat(format);

	// error handler - retries after 200ms if we couldn't get the entire image
    retry_timer.setInterval(250);
	connect(&retry_timer, SIGNAL(timeout()), this, SLOT(retry()));
}

void ViewerWidget::retry() {
	update();
}

void ViewerWidget::initializeGL() {
    initializeOpenGLFunctions();

    glClearColor(0, 0, 0, 1);
    glMatrixMode(GL_PROJECTION);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
}

//void ViewerWidget::resizeGL(int w, int h)
//{
//}

void ViewerWidget::paintEvent(QPaintEvent *e) {
    if (enable_paint) QOpenGLWidget::paintEvent(e);
}

void ViewerWidget::paintGL() {
    bool loop = true;
    while (loop) {
        loop = false;
        texture_failed = false;

        if (multithreaded) retry_timer.stop();

        glClear(GL_COLOR_BUFFER_BIT);

        current_clips.clear();

        for (int i=0;i<sequence->clip_count();i++) {
            Clip* c = sequence->get_clip(i);

            // if clip starts within one second and/or hasn't finished yet
            if (c != NULL) {
                if (is_clip_active(c, panel_timeline->playhead)) {
                    // if thread is already working, we don't want to touch this,
                    // but we also don't want to hang the UI thread
                    if (!c->open) {
                        open_clip(c, multithreaded);
                    }

                    bool added = false;
                    for (int j=0;j<current_clips.size();j++) {
                        if (current_clips.at(j)->track < c->track) {
                            current_clips.insert(j, c);
                            added = true;
                            break;
                        }
                    }
                    if (!added) {
                        current_clips.append(c);
                    }
                } else if (c->open) {
                    close_clip(c);
                }
            }
        }

        bool render_audio = (panel_timeline->playing || force_audio);

        for (int i=0;i<current_clips.size();i++) {
            Clip* c = current_clips.at(i);

            if (!c->finished_opening) {
                qDebug() << "[WARNING] Tried to display clip" << i << "but it's closed";
                texture_failed = true;
            } else if (is_clip_active(c, panel_timeline->playhead)) {
                if (c->stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    // start preparing cache
                    get_clip_frame(c, panel_timeline->playhead);

                    if (c->texture == NULL) {
                        qDebug() << "[WARNING] Texture hasn't been created yet";
                        texture_failed = true;
                    } else if (panel_timeline->playhead >= c->timeline_in) {
                        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                        glColor4f(1.0, 1.0, 1.0, 1.0);

                        glLoadIdentity();
                        int half_width = c->sequence->width/2;
                        int half_height = c->sequence->height/2;
                        if (flip) {
                            glOrtho(-half_width, half_width, -half_height, half_height, -1, 1);
                        } else {
                            glOrtho(-half_width, half_width, half_height, -half_height, -1, 1);
                        }
                        int anchor_x = c->media_stream->video_width/2;
                        int anchor_y = c->media_stream->video_height/2;

                        // perform all transform effects
                        for (int j=0;j<c->effects.size();j++) {
                            c->effects.at(j)->process_gl(&anchor_x, &anchor_y);
                        }

                        if (c->opening_transition != NULL) {
                            int transition_progress = panel_timeline->playhead-c->timeline_in;
                            if (transition_progress < c->opening_transition->length) {
                                c->opening_transition->process_transition((float)transition_progress/(float)c->opening_transition->length);
                            }
                        }
                        if (c->closing_transition != NULL) {
                            int transition_progress = c->closing_transition->length-(panel_timeline->playhead-c->timeline_in-c->getLength()+c->closing_transition->length);
                            if (transition_progress < c->closing_transition->length) {
                                c->closing_transition->process_transition((float)transition_progress/(float)c->closing_transition->length);
                            }
                        }

                        int anchor_right = c->media_stream->video_width - anchor_x;
                        int anchor_bottom = c->media_stream->video_height - anchor_y;

                        c->texture->bind();

                        glBegin(GL_QUADS);
                        glTexCoord2f(0.0, 0.0);
                        glVertex2f(-anchor_x, -anchor_y);
                        glTexCoord2f(1.0, 0.0);
                        glVertex2f(anchor_right, -anchor_y);
                        glTexCoord2f(1.0, 1.0);
                        glVertex2f(anchor_right, anchor_bottom);
                        glTexCoord2f(0.0, 1.0);
                        glVertex2f(-anchor_x, anchor_bottom);
                        glEnd();

                        c->texture->release();
                    }
                } else if (render_audio &&
                           c->stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                           c->lock.tryLock()) {
                    // clip is not caching, start caching audio
                    cache_clip(c, panel_timeline->playhead, false, false, c->reset_audio);
                    c->lock.unlock();
                }
            }
        }

        if (panel_timeline->playing) {
            int adjusted_read_index = audio_ibuffer_read%audio_ibuffer_size;
            int max_write = audio_ibuffer_size - adjusted_read_index;
            int actual_write = audio_io_device->write((const char*) audio_ibuffer+adjusted_read_index, max_write);
            audio_ibuffer_read += actual_write;

            // send samples to audio monitor cache
            if (panel_timeline->ui->audio_monitor->sample_cache_offset == -1) {
                panel_timeline->ui->audio_monitor->sample_cache_offset = panel_timeline->playhead;
            }
            long sample_cache_playhead = panel_timeline->ui->audio_monitor->sample_cache_offset + panel_timeline->ui->audio_monitor->sample_cache.size();
            int next_buffer_offset, buffer_offset_adjusted, i;
            int buffer_offset = get_buffer_offset_from_frame(sample_cache_playhead);
            samples.resize(av_get_channel_layout_nb_channels(sequence->audio_layout));
            samples.fill(0);
            while (buffer_offset < audio_ibuffer_read) {
                sample_cache_playhead++;
                next_buffer_offset = qMin(get_buffer_offset_from_frame(sample_cache_playhead), audio_ibuffer_read);
                while (buffer_offset < next_buffer_offset) {
                    for (i=0;i<samples.size();i++) {
                        buffer_offset_adjusted = buffer_offset%audio_ibuffer_size;
                        samples[i] = qMax(qAbs((qint16) (((audio_ibuffer[buffer_offset_adjusted+1] & 0xFF) << 8) | (audio_ibuffer[buffer_offset_adjusted] & 0xFF))), samples[i]);
                        buffer_offset += 2;
                    }
                }
                for (i=0;i<samples.size();i++) {
                    panel_timeline->ui->audio_monitor->sample_cache.append(samples.at(i));
                }
                buffer_offset = next_buffer_offset;
            }

            memset(audio_ibuffer+adjusted_read_index, 0, actual_write);
            if (actual_write == max_write) {
                // got all the bytes, write again
                actual_write = audio_io_device->write((const char*) audio_ibuffer, audio_ibuffer_size);
                memset(audio_ibuffer, 0, actual_write);
                audio_ibuffer_read += actual_write;
            }
        }

        if (texture_failed) {
            if (multithreaded) {
                retry_timer.start();
            } else {
                qDebug() << "[INFO] Texture failed - looping";
                loop = true;
            }
        }
    }
}
