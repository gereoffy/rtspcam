#!/usr/bin/env python3
"""rotate.py - deletes old recordings of the C recorder (src/rtspcam): run it daily from cron.

    rotate.py /srv/cameras                 # root = the -o directory of the recorder (the player's root)
    rotate.py /srv/cameras --dry-run       # only says what it would delete
    rotate.py /srv/cameras --force         # without the per-run limit (to catch up at once)

Every run, in this order:
  1. empties the trash (<root>/.deleted/: what was deleted in the player) completely, whatever the other rules say;
  2. by age, per camera: the recordings of the days older than the camera's keep days are deleted
     ("keep_days": 3 = today and the 2 days before; default 30), at most "max_days_per_run" days (2) per camera in one run
     (after a few days' stop the backlog goes in a few runs, and a broken clock cannot wipe everything at once);
  3. by free space: while the disk has less free space than "min_free_gb" (default 1), the oldest recordings go, whatever
     the camera (no per-run limit here).
Never deleted: a marked recording (a <name>.keep next to it: the player's star), and per camera the newest
"min_days" (3) day folders that exist (not calendar days: after a stop the last 3 days with recordings stay).
Empty day folders are removed. A recording = its .mp4, .mvmap, .mvvec (and a crashed .part).

Settings: configs/rotate.conf next to this script (--config), read on every run (JSON; not .json: the cameras'
configs are configs/<camera>.json, a camera called "rotate" would share it), e.g.:
    {"keep_days": 14, "keep_days_camera": {"Utca": 3, "Utca3": 3, "Szeru2": 30},
     "min_free_gb": 10, "max_days_per_run": 2, "min_days": 3}
Without it (or for a key it leaves out) the lenient defaults: keep_days 30, min_free_gb 1, max_days_per_run 2, min_days 3.
A missing config is written with these defaults and every camera of the root listed (30 days each), to be edited.
A config that cannot be read stops the run (nothing is deleted with settings that are not the intended ones).
Standard library only (Python 3.5+).
"""
import argparse
import collections
import datetime
import json
import os
import re
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
TRASH = ".deleted"
# the files of one recording, by suffix (the recorder writes <name>.mp4 + .mvmap + .mvvec; .part while it writes)
SUFFIXES = (".mp4", ".mvmap", ".mvvec", ".mp4.part", ".mvmap.part", ".mvvec.part")
# without a config (or for a key it leaves out): lenient, 30 days and 1 GB
DEFAULTS = {"keep_days": 30, "keep_days_camera": {}, "min_free_gb": 1, "max_days_per_run": 2, "min_days": 3}


def log(msg):
    print("%s %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg), flush=True)


def gb(n):
    return "%.2f GB" % (n / 1e9) if n >= 1e9 else "%.0f MB" % (n / 1e6)


def stem_of(fn):
    for suf in SUFFIXES:
        if fn.endswith(suf):
            return fn[:-len(suf)]
    return None                         # not a recording file (e.g. .keep): never touched here


class Rotator:
    def __init__(self, root, cfg, dry, force):
        self.root, self.cfg, self.dry, self.force = os.path.realpath(root), cfg, dry, force
        self.n = {"trash": 0, "age": 0, "space": 0}
        self.bytes = {"trash": 0, "age": 0, "space": 0}

    def remove(self, path, why):
        try:
            size = os.path.getsize(path)
        except OSError:
            return 0
        if not self.dry:
            try:
                os.remove(path)
            except OSError as e:
                log("cannot delete %s: %s" % (path, e))
                return 0
        self.bytes[why] += size
        return size

    # ---- 1. the trash ----
    def empty_trash(self):
        t = os.path.join(self.root, TRASH)
        if not os.path.isdir(t):
            return
        for d, dirs, files in os.walk(t, topdown=False):
            for fn in files:
                if fn.endswith(".mp4"):
                    self.n["trash"] += 1
                self.remove(os.path.join(d, fn), "trash")
            if not self.dry and d != t:
                try:
                    os.rmdir(d)
                except OSError:
                    pass

    # ---- the recordings ----
    def cameras(self):
        out = {}
        for cam in sorted(os.listdir(self.root)):
            cdir = os.path.join(self.root, cam)
            if cam.startswith(".") or not os.path.isdir(cdir):
                continue
            days = sorted(d for d in os.listdir(cdir) if DATE.match(d) and os.path.isdir(os.path.join(cdir, d)))
            if days:
                out[cam] = days
        return out

    def recordings(self, cam, day):
        """{stem: [files]} of one day folder, without the marked ones; and the number of marked ones"""
        ddir = os.path.join(self.root, cam, day)
        names = os.listdir(ddir)
        marked = set(fn[:-5] for fn in names if fn.endswith(".keep"))
        recs = {}
        for fn in names:
            st = stem_of(fn)
            if st is not None and st not in marked:
                recs.setdefault(st, []).append(os.path.join(ddir, fn))
        return recs, len(marked)

    def delete_rec(self, files, why):
        size = 0
        for f in files:
            size += self.remove(f, why)
        self.n[why] += 1
        return size

    def tidy(self, cam, day):
        ddir = os.path.join(self.root, cam, day)
        if not self.dry:
            try:
                os.rmdir(ddir)          # only if empty (marked recordings stay)
            except OSError:
                pass

    # ---- 2. by age ----
    def by_age(self, cams):
        today = datetime.date.today()
        mind = max(1, int(self.cfg["min_days"]))
        for cam, days in cams.items():
            keep = int(self.cfg["keep_days_camera"].get(cam, self.cfg["keep_days"]))
            cutoff = (today - datetime.timedelta(days=max(1, keep) - 1)).isoformat()
            protected = set(days[-mind:])
            # (only days with something to delete: an old day kept for its marked recordings must not take a place of
            # the per-run limit every day, or a few of them would stop the deletion for good)
            expired = []
            for d in days:
                if d < cutoff and d not in protected:
                    recs, marked = self.recordings(cam, d)
                    if recs:
                        expired.append((d, recs, marked))
            if not expired:
                continue
            todo = expired if self.force else expired[:max(1, int(self.cfg["max_days_per_run"]))]
            if len(todo) < len(expired):
                log("%s: %d day(s) are past the %d days, %d now, the rest in the next runs (--force: all at once)"
                    % (cam, len(expired), keep, len(todo)))
            for day, recs, marked in todo:
                size = sum(self.delete_rec(files, "age") for files in recs.values())
                log("%s %s: %d recording(s), %s%s (older than %d days)%s"
                    % (cam, day, len(recs), gb(size), " would be deleted" if self.dry else " deleted", keep,
                       ", %d marked kept" % marked if marked else ""))
                self.tidy(cam, day)

    # ---- 3. by free space ----
    def by_space(self):
        need = float(self.cfg["min_free_gb"]) * 1e9
        free = shutil.disk_usage(self.root).free
        if free >= need:
            return
        log("free space %s is below %s: the oldest recordings go" % (gb(free), gb(need)))
        mind = max(1, int(self.cfg["min_days"]))
        cand = []
        for cam, days in self.cameras().items():
            for day in days[:-mind]:                 # (the newest min_days folders stay)
                recs, _ = self.recordings(cam, day)
                for st, files in recs.items():
                    cand.append((day, st, cam, files))
        cand.sort()
        touched = set()
        for day, st, cam, files in cand:
            if free >= need:
                break
            free += self.delete_rec(files, "space")
            touched.add((cam, day))
        for cam, day in sorted(touched):
            self.tidy(cam, day)
        if free < need:
            log("still only %s free: nothing more may be deleted (marked recordings, the newest %d days)" % (gb(free), mind))
        else:
            log("free space now %s%s" % (gb(free), " (estimated)" if self.dry else ""))

    def run(self):
        self.empty_trash()
        self.by_age(self.cameras())
        self.by_space()
        total = sum(self.n.values())
        if total or self.dry:
            log("%s: %d recording(s), %s (trash %d, age %d, space %d)"
                % ("would delete" if self.dry else "deleted", total, gb(sum(self.bytes.values())),
                   self.n["trash"], self.n["age"], self.n["space"]))


def write_default_config(path, root, dry):
    """the config with the defaults and every camera of the root (with the default days), so that it only needs editing"""
    cams = sorted(c for c in os.listdir(root) if not c.startswith(".") and os.path.isdir(os.path.join(root, c)))
    cfg = collections.OrderedDict([("keep_days", DEFAULTS["keep_days"]),
                                   ("keep_days_camera", collections.OrderedDict((c, DEFAULTS["keep_days"]) for c in cams)),
                                   ("min_free_gb", DEFAULTS["min_free_gb"]), ("max_days_per_run", DEFAULTS["max_days_per_run"]),
                                   ("min_days", DEFAULTS["min_days"])])
    if dry:
        log("no config: %s would be written with the defaults (%d cameras, %d days each)" % (path, len(cams), DEFAULTS["keep_days"]))
        return
    try:
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path + ".tmp", "w") as fh:
            json.dump(cfg, fh, indent=2)
            fh.write("\n")
        os.replace(path + ".tmp", path)
        log("no config: %s written with the defaults (%d cameras, %d days each): edit it" % (path, len(cams), DEFAULTS["keep_days"]))
    except OSError as e:
        log("no config, and %s cannot be written (%s): the defaults are used" % (path, e))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="the recordings root (the -o directory of the recorder)")
    ap.add_argument("--config", default=os.path.join(HERE, "configs", "rotate.conf"))
    ap.add_argument("--dry-run", action="store_true", help="only say what would be deleted")
    ap.add_argument("--force", action="store_true", help="no per-run limit of the deletion by age")
    ap.add_argument("--keep-days", type=int, help="override the default keep days (not the per-camera ones)")
    ap.add_argument("--min-free-gb", type=float, help="override min_free_gb")
    a = ap.parse_args()
    cfg = dict(DEFAULTS)
    if not os.path.exists(a.config) and os.path.isdir(a.root):
        write_default_config(a.config, a.root, a.dry_run)
    if os.path.exists(a.config):
        try:
            with open(a.config) as fh:
                c = json.load(fh)
            if not isinstance(c, dict) or not isinstance(c.get("keep_days_camera", {}), dict):
                raise ValueError("not an object of the expected shape")
            cfg.update(c)
            for k in ("keep_days", "min_free_gb", "max_days_per_run", "min_days"):
                float(cfg[k])
            for v in cfg["keep_days_camera"].values():
                int(v)
        except (OSError, ValueError, TypeError) as e:
            sys.exit("rotate: config %s cannot be used (%s): nothing deleted" % (a.config, e))
    if a.keep_days is not None:
        cfg["keep_days"] = a.keep_days
    if a.min_free_gb is not None:
        cfg["min_free_gb"] = a.min_free_gb
    if not os.path.isdir(a.root):
        sys.exit("rotate: no such directory: %s" % a.root)
    Rotator(a.root, cfg, a.dry_run, a.force).run()


if __name__ == "__main__":
    main()
