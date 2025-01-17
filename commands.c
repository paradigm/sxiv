/* sxiv: commands.c
 * Copyright (c) 2011 Bert Muennich <muennich at informatik.hu-berlin.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "commands.h"
#include "image.h"
#include "thumbs.h"
#include "util.h"

void cleanup();
void remove_file(int, bool);
void load_image(int);
void redraw();
void reset_cursor();
void animate();
void slideshow();
void set_timeout(timeout_f, int, bool);
void reset_timeout(timeout_f);

extern appmode_t mode;
extern img_t img;
extern tns_t tns;
extern win_t win;

extern fileinfo_t *files;
extern int filecnt, fileidx;

bool it_quit(arg_t a) {
	cleanup();
	exit(EXIT_SUCCESS);
}

bool it_switch_mode(arg_t a) {
	if (mode == MODE_IMAGE) {
		if (tns.thumbs == NULL)
			tns_init(&tns, filecnt, &win);
		img_close(&img, false);
		reset_timeout(reset_cursor);
		if (img.slideshow) {
			img.slideshow = false;
			reset_timeout(slideshow);
		}
		tns.sel = fileidx;
		tns.dirty = true;
		mode = MODE_THUMB;
	} else {
		load_image(tns.sel);
		mode = MODE_IMAGE;
	}
	return true;
}

bool it_toggle_fullscreen(arg_t a) {
	win_toggle_fullscreen(&win);
	set_timeout(redraw, TO_REDRAW_RESIZE, false);
	if (mode == MODE_IMAGE)
		img.checkpan = true;
	else
		tns.dirty = true;
	return false;
}

bool it_reload_image(arg_t a) {
	if (mode == MODE_IMAGE) {
		load_image(fileidx);
	} else {
		win_set_cursor(&win, CURSOR_WATCH);
		if (!tns_load(&tns, tns.sel, &files[tns.sel], true, false)) {
			remove_file(tns.sel, false);
			tns.dirty = true;
			if (tns.sel >= tns.cnt)
				tns.sel = tns.cnt - 1;
		}
	}
	return true;
}

bool it_remove_image(arg_t a) {
	if (mode == MODE_IMAGE) {
		remove_file(fileidx, true);
		load_image(fileidx >= filecnt ? filecnt - 1 : fileidx);
		return true;
	} else if (tns.sel < tns.cnt) {
		remove_file(tns.sel, true);
		tns.dirty = true;
		if (tns.sel >= tns.cnt)
			tns.sel = tns.cnt - 1;
		return true;
	} else {
		return false;
	}
}

bool i_navigate(arg_t a) {
	long n = (long) a;

	if (mode == MODE_IMAGE) {
		n += fileidx;
		if (n < 0)
			n = 0;
		if (n >= filecnt)
			n = filecnt - 1;

		if (n != fileidx) {
			load_image(n);
			return true;
		}
	}
	return false;
}

bool it_first(arg_t a) {
	if (mode == MODE_IMAGE && fileidx != 0) {
		load_image(0);
		return true;
	} else if (mode == MODE_THUMB && tns.sel != 0) {
		tns.sel = 0;
		tns.dirty = true;
		return true;
	} else {
		return false;
	}
}

bool it_last(arg_t a) {
	if (mode == MODE_IMAGE && fileidx != filecnt - 1) {
		load_image(filecnt - 1);
		return true;
	} else if (mode == MODE_THUMB && tns.sel != tns.cnt - 1) {
		tns.sel = tns.cnt - 1;
		tns.dirty = true;
		return true;
	} else {
		return false;
	}
}

bool i_navigate_frame(arg_t a) {
	if (mode == MODE_IMAGE && !img.multi.animate)
		return img_frame_navigate(&img, (long) a);
	else
		return false;
}

bool i_toggle_animation(arg_t a) {
	if (mode != MODE_IMAGE)
		return false;

	if (img.multi.animate) {
		reset_timeout(animate);
		img.multi.animate = false;
	} else if (img_frame_animate(&img, true)) {
		set_timeout(animate, img.multi.frames[img.multi.sel].delay, true);
	}
	return true;
}

bool it_move(arg_t a) {
	direction_t dir = (direction_t) a;

	if (mode == MODE_IMAGE)
		return img_pan(&img, dir, false);
	else
		return tns_move_selection(&tns, dir);
}

bool i_pan_screen(arg_t a) {
	direction_t dir = (direction_t) a;

	if (mode == MODE_IMAGE)
		return img_pan(&img, dir, true);
	else
		return false;
}

bool i_pan_edge(arg_t a) {
	direction_t dir = (direction_t) a;

	if (mode == MODE_IMAGE)
		return img_pan_edge(&img, dir);
	else
		return false;
}

/* Xlib helper function for i_drag() */
Bool is_motionnotify(Display *d, XEvent *e, XPointer a) {
	return e != NULL && e->type == MotionNotify;
}

bool i_drag(arg_t a) {
	int dx = 0, dy = 0, i, ox, oy, x, y;
	unsigned int ui;
	bool dragging = true, next = false;
	XEvent e;
	Window w;

	if (mode != MODE_IMAGE)
		return false;
	if (!XQueryPointer(win.env.dpy, win.xwin, &w, &w, &i, &i, &ox, &oy, &ui))
		return false;
	
	win_set_cursor(&win, CURSOR_HAND);

	while (dragging) {
		if (!next)
			XMaskEvent(win.env.dpy,
			           ButtonPressMask | ButtonReleaseMask | PointerMotionMask, &e);
		switch (e.type) {
			case ButtonPress:
			case ButtonRelease:
				dragging = false;
				break;
			case MotionNotify:
				x = e.xmotion.x;
				y = e.xmotion.y;
				if (x >= 0 && x <= win.w && y >= 0 && y <= win.h) {
					dx += x - ox;
					dy += y - oy;
				}
				ox = x;
				oy = y;
				break;
		}
		if (dragging)
			next = XCheckIfEvent(win.env.dpy, &e, is_motionnotify, None);
		if ((!dragging || !next) && (dx != 0 || dy != 0)) {
			if (img_move(&img, dx, dy))
				img_render(&img);
			dx = dy = 0;
		}
	}
	
	win_set_cursor(&win, CURSOR_ARROW);
	set_timeout(reset_cursor, TO_CURSOR_HIDE, true);
	reset_timeout(redraw);

	return false;
}

bool i_zoom(arg_t a) {
	long scale = (long) a;

	if (mode != MODE_IMAGE)
		return false;

	if (scale > 0)
		return img_zoom_in(&img);
	else if (scale < 0)
		return img_zoom_out(&img);
	else
		return img_zoom(&img, 1.0);
}

bool i_fit_to_win(arg_t a) {
	bool ret = false;

	if (mode == MODE_IMAGE) {
		if ((ret = img_fit_win(&img)))
			img_center(&img);
	}
	return ret;
}

bool i_fit_to_img(arg_t a) {
	int x, y;
	unsigned int w, h;
	bool ret = false;

	if (mode == MODE_IMAGE) {
		x = MAX(0, win.x + img.x);
		y = MAX(0, win.y + img.y);
		w = img.w * img.zoom;
		h = img.h * img.zoom;
		if ((ret = win_moveresize(&win, x, y, w, h))) {
			img.x = x - win.x;
			img.y = y - win.y;
		}
	}
	return ret;
}

bool i_rotate(arg_t a) {
	direction_t dir = (direction_t) a;

	if (mode == MODE_IMAGE) {
		if (dir == DIR_LEFT) {
			img_rotate_left(&img);
			return true;
		} else if (dir == DIR_RIGHT) {
			img_rotate_right(&img);
			return true;
		}
	}
	return false;
}

bool i_toggle_slideshow(arg_t a) {
	if (mode == MODE_IMAGE) {
		if (img.slideshow) {
			img.slideshow = false;
			reset_timeout(slideshow);
			return true;
		} else if (fileidx + 1 < filecnt) {
			img.slideshow = true;
			set_timeout(slideshow, img.ss_delay, true);
			return true;
		}
	}
	return false;
}

bool i_adjust_slideshow(arg_t a) {
	long d = (long) a;
	int i, delays[] = { 1, 2, 3, 5, 10, 15, 20, 30, 60, 120, 180, 300, 600 };

	if (mode != MODE_IMAGE || !img.slideshow)
		return false;

	if (d < 0) {
		for (i = ARRLEN(delays) - 2; i >= 0; i--) {
			if (img.ss_delay > delays[i] * 1000) {
				img.ss_delay = delays[i] * 1000;
				return true;
			}
		}
	} else {
		for (i = 1; i < ARRLEN(delays); i++) {
			if (img.ss_delay < delays[i] * 1000) {
				img.ss_delay = delays[i] * 1000;
				return true;
			}
		}
	}
	return false;
}

bool i_toggle_antialias(arg_t a) {
	if (mode == MODE_IMAGE) {
		img_toggle_antialias(&img);
		return true;
	} else {
		return false;
	}
}

bool it_toggle_alpha(arg_t a) {
	img.alpha = tns.alpha = !img.alpha;
	if (mode == MODE_IMAGE)
		img.dirty = true;
	else
		tns.dirty = true;
	return true;
}

bool it_open_with(arg_t a) {
	const char *prog = (const char*) a;
	pid_t pid;

	if (prog == NULL || *prog == '\0')
		return false;

	if ((pid = fork()) == 0) {
		execlp(prog, prog,
		       files[mode == MODE_IMAGE ? fileidx : tns.sel].path, NULL);
		warn("could not exec: %s", prog);
		exit(EXIT_FAILURE);
	} else if (pid < 0) {
		warn("could not fork. program was: %s", prog);
	}
	return false;
}

bool it_shell_cmd(arg_t a) {
	int n, status;
	const char *cmdline = (const char*) a;
	pid_t pid;

	if (cmdline == NULL || *cmdline == '\0')
		return false;

	n = mode == MODE_IMAGE ? fileidx : tns.sel;

	if (setenv("SXIV_IMG", files[n].path, 1) < 0) {
		warn("could not set env.-variable: SXIV_IMG. command line was: %s",
		     cmdline);
		return false;
	}

	if ((pid = fork()) == 0) {
		execl("/bin/sh", "/bin/sh", "-c", cmdline, NULL);
		warn("could not exec: /bin/sh. command line was: %s", cmdline);
		exit(EXIT_FAILURE);
	} else if (pid < 0) {
		warn("could not fork. command line was: %s", cmdline);
		return false;
	}

	win_set_cursor(&win, CURSOR_WATCH);

	waitpid(pid, &status, 0);
	if (WIFEXITED(status) == 0 || WEXITSTATUS(status) != 0)
		warn("child exited with non-zero return value: %d. command line was: %s",
		     WEXITSTATUS(status), cmdline);
	
	if (mode == MODE_IMAGE) {
		img_close(&img, true);
		load_image(fileidx);
	}
	if (!tns_load(&tns, n, &files[n], true, mode == MODE_IMAGE) &&
	    mode == MODE_THUMB)
	{
		remove_file(tns.sel, false);
		tns.dirty = true;
		if (tns.sel >= tns.cnt)
			tns.sel = tns.cnt - 1;
	}
	return true;
}
