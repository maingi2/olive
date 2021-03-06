#include "timeline.h"
#include "ui_timeline.h"

#include "panels/panels.h"
#include "panels/project.h"
#include "panels/effectcontrols.h"
#include "ui/timelinewidget.h"
#include "project/sequence.h"
#include "project/clip.h"
#include "ui/viewerwidget.h"
#include "playback/audio.h"
#include "panels/viewer.h"
#include "playback/cacher.h"
#include "playback/playback.h"
#include "effects/transition.h"
#include "ui_viewer.h"
#include "project/undo.h"

#include <QTime>
#include <QScrollBar>
#include <QtMath>

Timeline::Timeline(QWidget *parent) :
	QDockWidget(parent),
	ui(new Ui::Timeline)
{    
    selecting = false;
    moving_init = false;
    moving_proc = false;
    splitting = false;
    importing = false;
    playing = false;
    trim_in_point = false;
    snapped = false;
    rect_select_init = false;
    rect_select_proc = false;
    edit_tool_selects_links = false;
    edit_tool_also_seeks = false;
    select_also_seeks = false;
    paste_seeks = true;
    snapping = true;
    last_frame = 0;
    playhead = 0;
    snap_point = 0;
    cursor_frame = 0;
    cursor_track = 0;
	trim_target = -1;

	ui->setupUi(this);

	ui->video_area->bottom_align = true;

	tool_buttons.append(ui->toolArrowButton);
	tool_buttons.append(ui->toolEditButton);
	tool_buttons.append(ui->toolRippleButton);
	tool_buttons.append(ui->toolRazorButton);
	tool_buttons.append(ui->toolSlipButton);
	tool_buttons.append(ui->toolRollingButton);
    tool_buttons.append(ui->toolSlideButton);

	ui->toolArrowButton->click();

	zoom = 1.0f;

    update_sequence();

    connect(&playback_updater, SIGNAL(timeout()), this, SLOT(repaint_timeline()));
}

Timeline::~Timeline()
{
	delete ui;
}

void Timeline::go_to_start() {
	seek(0);
}

void Timeline::previous_frame() {
	seek(playhead-1);
}

void Timeline::next_frame() {
	seek(playhead+1);
}

void Timeline::previous_cut() {
    if (playhead > 0) {
        long p_cut = 0;
        for (int i=0;i<sequence->clip_count();i++) {
            Clip* c = sequence->get_clip(i);
            if (c != NULL) {
                if (c->timeline_out > p_cut && c->timeline_out < playhead) {
                    p_cut = c->timeline_out;
                } else if (c->timeline_in > p_cut && c->timeline_in < playhead) {
                    p_cut = c->timeline_in;
                }
            }
        }
        seek(p_cut);
    }
}

void Timeline::next_cut() {
    bool seek_enabled = false;
    long n_cut = LONG_MAX;
    for (int i=0;i<sequence->clip_count();i++) {
        Clip* c = sequence->get_clip(i);
        if (c != NULL) {
            if (c->timeline_in < n_cut && c->timeline_in > playhead) {
                n_cut = c->timeline_in;
                seek_enabled = true;
            } else if (c->timeline_out < n_cut && c->timeline_out > playhead) {
                n_cut = c->timeline_out;
                seek_enabled = true;
            }
        }
    }
    if (seek_enabled) seek(n_cut);
}

void Timeline::reset_all_audio() {
    // reset all clip audio
    for (int i=0;i<sequence->clip_count();i++) {
        Clip* c = sequence->get_clip(i);
        if (c != NULL) {
            c->reset_audio = true;
            c->frame_sample_index = 0;
            c->audio_buffer_write = 0;
        }
    }
    ui->audio_monitor->reset();
    clear_audio_ibuffer();
}

void Timeline::seek(long p) {
    pause();

    reset_all_audio();
    audio_ibuffer_frame = p;

	playhead = p;

    repaint_timeline();
}

void Timeline::toggle_play() {
	if (playing) {
		pause();
	} else {
		play();
    }
}

void Timeline::play() {
    playhead_start = playhead;
    start_msecs = QDateTime::currentMSecsSinceEpoch();
	playback_updater.start();
    playing = true;
    panel_viewer->set_playpause_icon(false);
}

void Timeline::pause() {
	playing = false;
    panel_viewer->set_playpause_icon(true);
}

void Timeline::go_to_end() {
	seek(sequence->getEndFrame());
}

int Timeline::get_track_height_size(bool video) {
    if (video) {
        return video_track_heights.size();
    } else {
        return audio_track_heights.size();
    }
}

void Timeline::add_transition() {
    for (int i=0;i<sequence->clip_count();i++) {
        Clip* c = sequence->get_clip(i);
        if (c != NULL && is_clip_selected(c, true)) {
            if (c->opening_transition == NULL) {
                c->opening_transition = create_transition(0, c);
            }
            if (c->closing_transition == NULL) {
                c->closing_transition = create_transition(0, c);
            }
        }
    }
    redraw_all_clips(true);
}

int Timeline::calculate_track_height(int track, int value) {
    int index = (track < 0) ? qAbs(track + 1) : track;
    QVector<int>& vector = (track < 0) ? video_track_heights : audio_track_heights;
    while (vector.size() < index+1) {
        vector.append(TRACK_DEFAULT_HEIGHT);
    }
    if (value > -1) {
        vector[index] = value;
    }
    return vector.at(index);
}

void Timeline::update_sequence() {
    bool null_sequence = (sequence == NULL);

	for (int i=0;i<tool_buttons.count();i++) {
		tool_buttons[i]->setEnabled(!null_sequence);
	}
    ui->snappingButton->setEnabled(!null_sequence);
	ui->pushButton_4->setEnabled(!null_sequence);
	ui->pushButton_5->setEnabled(!null_sequence);
    ui->headers->setEnabled(!null_sequence);
	ui->video_area->setEnabled(!null_sequence);
	ui->audio_area->setEnabled(!null_sequence);

	if (null_sequence) {
		setWindowTitle("Timeline: <none>");
	} else {
		setWindowTitle("Timeline: " + sequence->name);
        redraw_all_clips(false);
        playback_updater.setInterval(qFloor(1000 / sequence->frame_rate));
        reset_all_audio();
	}
}

int Timeline::get_snap_range() {
    return getFrameFromScreenPoint(10);
}

bool Timeline::focused() {
    return (ui->headers->hasFocus() || ui->video_area->hasFocus() || ui->audio_area->hasFocus());
}

void Timeline::repaint_timeline() {
    if (playing) {
        playhead = round(playhead_start + ((QDateTime::currentMSecsSinceEpoch()-start_msecs) * 0.001 * sequence->frame_rate));
	}
    ui->headers->update();
	ui->video_area->update();
	ui->audio_area->update();
    if (last_frame != playhead) {
		panel_viewer->viewer_widget->update();
        ui->audio_monitor->update();
		last_frame = playhead;
	}
    panel_viewer->update_playhead_timecode();
}

void Timeline::redraw_all_clips(bool changed) {
    if (sequence != NULL) {
        if (changed) {
            project_changed = true;
            if (!playing) reset_all_audio();
            panel_viewer->viewer_widget->update();
        }

        ui->video_area->redraw_clips();
        ui->audio_area->redraw_clips();
        ui->headers->update();

        panel_viewer->update_end_timecode();
    }
}

void Timeline::select_all() {
	selections.clear();
	for (int i=0;i<sequence->clip_count();i++) {
        Clip* c = sequence->get_clip(i);
        Selection s;
        s.in = c->timeline_in;
        s.out = c->timeline_out;
        s.track = c->track;
        if (c != NULL) selections.append(s);
	}
    repaint_timeline();
}

void Timeline::delete_selection(bool ripple_delete) {
	if (selections.size() > 0) {
        panel_effect_controls->clear_effects(true);

        TimelineAction* ta = new TimelineAction();

		long ripple_point = selections.at(0).in;
		long ripple_length = selections.at(0).out - selections.at(0).in;

        // retrieve ripple_point and ripple_length from current selection
        for (int i=0;i<selections.size();i++) {
            const Selection& s = selections.at(i);
            if (ripple_delete) {
                if (ripple_point > s.in) ripple_point = s.in;
                if (ripple_length > s.out - s.in) ripple_length = s.out - s.in;
            }
        }
        delete_areas_and_relink(ta, selections);
		selections.clear();

		if (ripple_delete) {
			long validator;
			for (int i=0;i<sequence->clip_count();i++) {
				// check every clip after and see if it'll collide
				// NOTE, we could probably re-use the validation code for the ripple tool here for optimization (since it's technically better code I think)
                Clip* c = sequence->get_clip(i);
                if (c != NULL && c->timeline_in >= ripple_point) {
					for (int j=0;j<sequence->clip_count();j++) {
                        Clip* cc = sequence->get_clip(j);
                        if (cc != NULL && cc->timeline_in < ripple_point) {
                            validator = c->timeline_in - ripple_length - cc->timeline_out;
							if (validator < 0) ripple_length += validator;

							if (ripple_length <= 0) {
								// we've seen all we need to see here (can't ripple so stop looping)
								i = j = sequence->clip_count();
							}
						}
					}
				}
			}
            if (ripple_length > 0) ripple(ta, ripple_point, -ripple_length);
		}

        undo_stack.push(ta);

        redraw_all_clips(true);
	}
}

int lerp(int a, int b, double t) {
    return ((1.0 - t) * a) + (t * b);
}

void Timeline::set_zoom(bool in) {
    if (in) {
        zoom *= 2;
    } else {
        zoom /= 2;
    }
    ui->timeline_area->horizontalScrollBar()->setValue(
                lerp(
                    ui->timeline_area->horizontalScrollBar()->value(),
                    getScreenPointFromFrame(playhead) - (ui->timeline_area->width()/2),
                    0.99
                )
            );
    redraw_all_clips(false);
}

void Timeline::ripple(TimelineAction* ta, long ripple_point, long ripple_length) {
    // ripple all clips around the ripple_point
	for (int i=0;i<sequence->clip_count();i++) {
        Clip* c = sequence->get_clip(i);
        if (c != NULL && c->timeline_in >= ripple_point) {
            ta->increase_timeline_in(sequence, i, ripple_length);
            ta->increase_timeline_out(sequence, i, ripple_length);
		}
    }

    // ripple the selections
    for (int i=0;i<selections.size();i++) {
        Selection& s = selections[i];
        // only ripple the selection if it's within range of the ripple point
        if (s.old_in >= ripple_point) {
            s.in += ripple_length;
            s.out += ripple_length;
        }
    }
}

void Timeline::decheck_tool_buttons(QObject* sender) {
	for (int i=0;i<tool_buttons.count();i++) {
        tool_buttons[i]->setChecked(tool_buttons.at(i) == sender);
	}
}

QVector<int> Timeline::get_tracks_of_linked_clips(int i) {
    QVector<int> tracks;
    Clip* clip = sequence->get_clip(i);
    for (int j=0;j<clip->linked.size();j++) {
        tracks.append(sequence->get_clip(clip->linked.at(j))->track);
    }
    return tracks;
}

void Timeline::on_pushButton_4_clicked()
{
    set_zoom(true);
}

void Timeline::on_pushButton_5_clicked()
{
    set_zoom(false);
}

bool Timeline::is_clip_selected(Clip* clip, bool containing) {
	for (int i=0;i<selections.size();i++) {
		const Selection& s = selections.at(i);
        if (clip->track == s.track && ((clip->timeline_in >= s.in && clip->timeline_out <= s.out && containing) ||
                (!containing && !(clip->timeline_in < s.in && clip->timeline_out < s.in) && !(clip->timeline_in > s.in && clip->timeline_out > s.in)))) {
			return true;
		}
	}
	return false;
}

void Timeline::on_snappingButton_toggled(bool checked) {
    snapping = checked;
}

Clip* Timeline::split_clip(TimelineAction* ta, int p, long frame) {
    Clip* pre = sequence->get_clip(p);
    if (pre != NULL && pre->timeline_in < frame && pre->timeline_out > frame) { // guard against attempts to split at in/out points
        Clip* post = pre->copy();

        ta->set_timeline_out(sequence, p, frame);
        post->timeline_in = frame;
        post->clip_in = pre->clip_in + (frame - pre->timeline_in);

        long pre_length = pre->getLength();

        if (pre->closing_transition != NULL) {
            post->closing_transition = pre->closing_transition;
            pre->closing_transition = NULL;
            long post_length = post->getLength();
            if (post->closing_transition->length > post_length) {
                post->closing_transition->length = post_length;
            }
        }
        if (pre->opening_transition != NULL && pre->opening_transition->length > pre_length) {
            pre->opening_transition->length = pre_length;
        }

        return post;
    }
    return NULL;
}

bool Timeline::has_clip_been_split(int c) {
    for (int i=0;i<split_cache.size();i++) {
        if (split_cache.at(i) == c) {
            return true;
        }
    }
    return false;
}

bool Timeline::split_clip_and_relink(TimelineAction* ta, int clip, long frame, bool relink) {
    // see if we split this clip before
    if (has_clip_been_split(clip)) {
        return false;
    }

    split_cache.append(clip);

    Clip* c = sequence->get_clip(clip);
    if (c != NULL) {
        QVector<int> pre_clips;
        QVector<Clip*> post_clips;

        Clip* post = split_clip(ta, clip, frame);

        // if alt is not down, split clips links too
        if (post == NULL) {
            return false;
        } else {
            post_clips.append(post);
            if (relink) {
                pre_clips.append(clip);

                bool original_clip_is_selected = is_clip_selected(c, true);

                // find linked clips of old clip
                for (int i=0;i<c->linked.size();i++) {
                    int l = c->linked.at(i);
                    if (!has_clip_been_split(l)) {
                        Clip* link = sequence->get_clip(l);
                        if ((original_clip_is_selected && is_clip_selected(link, true)) || !original_clip_is_selected) {
                            split_cache.append(l);
                            Clip* s = split_clip(ta, l, frame);
                            if (s != NULL) {
                                pre_clips.append(l);
                                post_clips.append(s);
                            }
                        }
                    }
                }

                relink_clips_using_ids(pre_clips, post_clips);
            }
            ta->add_clips(sequence, post_clips);
            return true;
        }
    }
    return false;
}

void Timeline::clean_up_selections(QVector<Selection>& areas) {
    for (int i=0;i<areas.size();i++) {
        Selection& s = areas[i];
        for (int j=0;j<areas.size();j++) {
            if (i != j) {
                Selection& ss = areas[j];
                if (s.track == ss.track) {
                    bool remove = false;
                    if (s.in < ss.in && s.out > ss.out) {
                        // do nothing
                    } else if (s.in >= ss.in && s.out <= ss.out) {
                        remove = true;
                    } else if (s.in <= ss.out && s.out > ss.out) {
                        ss.out = s.out;
                        remove = true;
                    } else if (s.out >= ss.in && s.in < ss.in) {
                        ss.in = s.in;
                        remove = true;
                    }
                    if (remove) {
                        areas.removeAt(i);
                        i--;
                        break;
                    }
                }
            }
        }
    }
}

void Timeline::delete_areas_and_relink(TimelineAction* ta, QVector<Selection>& areas) {
    clean_up_selections(areas);

    QVector<int> pre_clips;
    QVector<Clip*> post_clips;

    for (int i=0;i<areas.size();i++) {
        const Selection& s = areas.at(i);
        for (int j=0;j<sequence->clip_count();j++) {
            Clip* c = sequence->get_clip(j);
            if (c != NULL && c->track == s.track && !c->undeletable) {
                if (c->timeline_in >= s.in && c->timeline_out <= s.out) {
                    // clips falls entirely within deletion area
                    ta->delete_clip(sequence, j);
                } else if (c->timeline_in < s.in && c->timeline_out > s.out) {
                    // middle of clip is within deletion area

                    // duplicate clip
                    Clip* post = c->copy();

                    ta->set_timeline_out(sequence, j, s.in);
                    post->timeline_in = s.out;
                    post->clip_in = c->clip_in + (s.in - c->timeline_in) + (s.out - s.in);

                    pre_clips.append(j);
                    post_clips.append(post);
                } else if (c->timeline_in < s.in && c->timeline_out > s.in) {
                    // only out point is in deletion area
                    ta->set_timeline_out(sequence, j, s.in);
                } else if (c->timeline_in < s.out && c->timeline_out > s.out) {
                    // only in point is in deletion area
                    ta->increase_clip_in(sequence, j, s.out - c->timeline_in);
                    ta->set_timeline_in(sequence, j, s.out);
                }
            }
        }
    }
    relink_clips_using_ids(pre_clips, post_clips);
    ta->add_clips(sequence, post_clips);
}

void Timeline::copy(bool del) {
    bool cleared = false;
    bool copied = false;

    long min_in = 0;

    for (int i=0;i<sequence->clip_count();i++) {
        Clip* c = sequence->get_clip(i);
        if (c != NULL) {
            for (int j=0;j<selections.size();j++) {
                const Selection& s = selections.at(j);
                if (s.track == c->track && !((c->timeline_in <= s.in && c->timeline_out <= s.in) || (c->timeline_in >= s.out && c->timeline_out >= s.out))) {
                    if (!cleared) {
                        clip_clipboard.clear();
                        cleared = true;
                    }

                    Clip* copied_clip = c->copy();

                    // copy linked IDs (we correct these later in paste())
                    copied_clip->linked = c->linked;

                    if (copied_clip->timeline_in < s.in) {
                        copied_clip->clip_in += (s.in - copied_clip->timeline_in);
                        copied_clip->timeline_in = s.in;
                    }

                    if (copied_clip->timeline_out > s.out) {
                        copied_clip->timeline_out = s.out;
                    }

                    if (copied) {
                        min_in = qMin(min_in, s.in);
                    } else {
                        min_in = s.in;
                        copied = true;
                    }

                    copied_clip->load_id = i;

                    clip_clipboard.append(copied_clip);
                }
            }
        }
    }

    for (int i=0;i<clip_clipboard.size();i++) {
        // initialize all timeline_ins to 0 or offsets of
        clip_clipboard[i]->timeline_in -= min_in;
        clip_clipboard[i]->timeline_out -= min_in;
    }

    if (del && copied) {
        delete_selection(false);
    }
}

void Timeline::relink_clips_using_ids(QVector<int>& old_clips, QVector<Clip*>& new_clips) {
    // relink pasted clips
    for (int i=0;i<old_clips.size();i++) {
        // these indices should correspond
        Clip* oc = sequence->get_clip(old_clips.at(i));
        for (int j=0;j<oc->linked.size();j++) {
            for (int k=0;k<old_clips.size();k++) { // find clip with that ID
                if (oc->linked.at(j) == old_clips.at(k)) {
                    new_clips.at(i)->linked.append(k);
                }
            }
        }
    }
}

void Timeline::paste() {
    if (clip_clipboard.size() > 0) {
        TimelineAction* ta = new TimelineAction();

        // create copies and delete areas that we'll be pasting to
        QVector<Selection> delete_areas;
        QVector<Clip*> pasted_clips;
        long paste_end = 0;
        for (int i=0;i<clip_clipboard.size();i++) {
            Clip* c = clip_clipboard.at(i);

            // create copy of clip and offset by playhead
            Clip* cc = c->copy();
            cc->sequence = sequence;
            cc->timeline_in += playhead;
            cc->timeline_out += playhead;
            cc->track = c->track;
            if (cc->timeline_out > paste_end) paste_end = cc->timeline_out;
            cc->sequence = sequence;
            pasted_clips.append(cc);

            Selection s;
            s.in = cc->timeline_in;
            s.out = cc->timeline_out;
            s.track = c->track;
            delete_areas.append(s);
        }
        delete_areas_and_relink(ta, delete_areas);

        // ADAPT
        for (int i=0;i<clip_clipboard.size();i++) {
            // these indices should correspond
            Clip* oc = clip_clipboard.at(i);

            for (int j=0;j<oc->linked.size();j++) {
                for (int k=0;k<clip_clipboard.size();k++) { // find clip with that ID
                    if (clip_clipboard.at(k)->load_id == oc->linked.at(j)) {
                        pasted_clips.at(i)->linked.append(k);
                    }
                }
            }
        }
        // ADAPT

        ta->add_clips(sequence, pasted_clips);

        undo_stack.push(ta);

        redraw_all_clips(true);

        if (paste_seeks) {
            seek(paste_end);
        }
    }
}

bool Timeline::split_selection(TimelineAction* ta) {
    bool split = false;

    // temporary relinking vectors
    QVector<int> pre_splits;
    QVector<Clip*> post_splits;
    QVector<Clip*> secondary_post_splits;

    // find clips within selection and split
    for (int j=0;j<sequence->clip_count();j++) {
        Clip* clip = sequence->get_clip(j);
        if (clip != NULL) {
            for (int i=0;i<selections.size();i++) {
                const Selection& s = selections.at(i);
                if (s.track == clip->track) {
                    if (clip->timeline_in < s.in && clip->timeline_out > s.out) {
                        Clip* split_A = clip->copy();
                        split_A->clip_in += (s.in - clip->timeline_in);
                        split_A->timeline_in = s.in;
                        split_A->timeline_out = s.out;
                        pre_splits.append(j);
                        post_splits.append(split_A);

                        Clip* split_B = clip->copy();
                        split_B->clip_in += (s.out - clip->timeline_in);
                        split_B->timeline_in = s.out;
                        secondary_post_splits.append(split_B);

                        ta->set_timeline_out(sequence, j, s.in);
                        split = true;
                    } else {
                        Clip* post_a = split_clip(ta, j, s.in);
                        Clip* post_b = split_clip(ta, j, s.out);
                        if (post_a != NULL) {
                            pre_splits.append(j);
                            post_splits.append(post_a);
                            split = true;
                        }
                        if (post_b != NULL) {
                            if (post_a != NULL) {
                                pre_splits.append(j);
                                post_splits.append(post_b);
                            } else {
                                secondary_post_splits.append(post_b);
                            }
                            split = true;
                        }
                    }
                }
            }
        }
    }

    if (split) {
        // relink after splitting
        relink_clips_using_ids(pre_splits, post_splits);
        relink_clips_using_ids(pre_splits, secondary_post_splits);
        ta->add_clips(sequence, post_splits);
        ta->add_clips(sequence, secondary_post_splits);

        return true;
    }
    return false;
}

void Timeline::split_at_playhead() {
    TimelineAction* ta = new TimelineAction();
    bool split_selected = false;
    split_cache.clear();

    if (selections.size() > 0) {
        // see if whole clips are selected
        QVector<int> pre_clips;
        QVector<Clip*> post_clips;
        for (int j=0;j<sequence->clip_count();j++) {
            Clip* clip = sequence->get_clip(j);
            if (clip != NULL && is_clip_selected(clip, true)) {
                Clip* s = split_clip(ta, j, playhead);
                if (s != NULL) {
                    pre_clips.append(j);
                    post_clips.append(s);
                    split_selected = true;
                }
            }
        }

        if (split_selected) {
            // relink clips if we split
            relink_clips_using_ids(pre_clips, post_clips);
            ta->add_clips(sequence, post_clips);
        } else {
            // split a selection if not
            split_selected = split_selection(ta);
        }
    }

    // if nothing was selected or no selections fell within playhead, simply split at playhead
    if (!split_selected) {
        for (int j=0;j<sequence->clip_count();j++) {
            Clip* c = sequence->get_clip(j);
            if (c != NULL) {
                // always relinks
                if (split_clip_and_relink(ta, j, playhead, true)) {
                    split_selected = true;
                }
            }
        }
    }

    if (split_selected) {
        undo_stack.push(ta);
        redraw_all_clips(true);
    } else {
        delete ta;
    }
}

bool Timeline::snap_to_point(long point, long* l) {
    int limit = get_snap_range();
    if (*l > point-limit-1 && *l < point+limit+1) {
        snap_point = point;
        *l = point;
        snapped = true;
        return true;
    }
    return false;
}

void Timeline::snap_to_clip(long* l, bool playhead_inclusive) {
    snapped = false;
    if (snapping) {
        if (!playhead_inclusive || !snap_to_point(playhead, l)) {
            for (int i=0;i<sequence->clip_count();i++) {
                Clip* c = sequence->get_clip(i);
                if (c != NULL) {
                    if (snap_to_point(c->timeline_in, l)) {
                        break;
                    } else if (snap_to_point(c->timeline_out, l)) {
                        break;
                    }
                }
            }
        }
    }
}

void Timeline::increase_track_height() {
    for (int i=0;i<video_track_heights.size();i++) {
        video_track_heights[i] += TRACK_HEIGHT_INCREMENT;
    }
    for (int i=0;i<audio_track_heights.size();i++) {
        audio_track_heights[i] += TRACK_HEIGHT_INCREMENT;
    }
    redraw_all_clips(false);
}

void Timeline::decrease_track_height() {
    for (int i=0;i<video_track_heights.size();i++) {
        video_track_heights[i] -= TRACK_HEIGHT_INCREMENT;
        if (video_track_heights[i] < TRACK_MIN_HEIGHT) video_track_heights[i] = TRACK_MIN_HEIGHT;
    }
    for (int i=0;i<audio_track_heights.size();i++) {
        audio_track_heights[i] -= TRACK_HEIGHT_INCREMENT;
        if (audio_track_heights[i] < TRACK_MIN_HEIGHT) audio_track_heights[i] = TRACK_MIN_HEIGHT;
    }
    redraw_all_clips(false);
}

void Timeline::deselect() {
    selections.clear();
    repaint_timeline();
}

long Timeline::getFrameFromScreenPoint(int x) {
    long f = round((float) x / zoom);
    if (f < 0) {
        return 0;
    }
    return f;
}

int Timeline::getScreenPointFromFrame(long frame) {
    return (int) round(frame*zoom);
}

void Timeline::on_toolArrowButton_clicked() {
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::ArrowCursor);
    tool = TIMELINE_TOOL_POINTER;
}

void Timeline::on_toolEditButton_clicked() {
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::IBeamCursor);
    tool = TIMELINE_TOOL_EDIT;
}

void Timeline::on_toolRippleButton_clicked() {
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::ArrowCursor);
    tool = TIMELINE_TOOL_RIPPLE;
}

void Timeline::on_toolRollingButton_clicked() {
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::ArrowCursor);
    tool = TIMELINE_TOOL_ROLLING;
}

void Timeline::on_toolRazorButton_clicked()
{
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::IBeamCursor);
    tool = TIMELINE_TOOL_RAZOR;
}

void Timeline::on_toolSlipButton_clicked()
{
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::ArrowCursor);
    tool = TIMELINE_TOOL_SLIP;
}

void Timeline::on_toolSlideButton_clicked()
{
    decheck_tool_buttons(sender());
    ui->timeline_area->setCursor(Qt::ArrowCursor);
    tool = TIMELINE_TOOL_SLIDE;
}
