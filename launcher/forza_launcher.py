#!/usr/bin/env python3
# forza-linux launcher: a Qt (PySide6) front end for build/forza-linux.
#
#   python3 launcher/forza_launcher.py                     open the launcher
#   python3 launcher/forza_launcher.py --install-shortcuts menu entry + desktop icon
#   python3 launcher/forza_launcher.py --print-command     what Play would run
#
# Settings: ~/.config/forza-linux/launcher.json. Session logs (the game's whole
# output): ~/.cache/forza-linux/logs/. The game art shown here, and the desktop
# icon --install-shortcuts makes, are read from the user's own game folder at
# run time; none of it is part of this repository (CLAUDE.md Rule 0).

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from PySide6.QtCore import (Property, QEasingCurve, QObject, QProcess, QProcessEnvironment, QPropertyAnimation,
                            QRectF, QSize, Qt, QTimer, QUrl, Signal)
from PySide6.QtGui import (QBrush, QColor, QDesktopServices, QFont, QFontDatabase, QGuiApplication, QIcon, QImage,
                           QLinearGradient, QPainter, QPainterPath, QPalette, QPen, QPixmap, QRadialGradient,
                           QSyntaxHighlighter, QTextCharFormat)
from PySide6.QtWidgets import (QAbstractButton, QAbstractItemView, QApplication, QButtonGroup, QCheckBox, QComboBox,
                               QFileDialog, QFrame, QGridLayout, QHBoxLayout, QHeaderView, QInputDialog, QLabel,
                               QLineEdit, QMainWindow, QMessageBox, QPlainTextEdit, QPushButton, QScrollArea,
                               QSizePolicy, QSlider, QSpinBox, QStackedWidget, QTableWidget, QTableWidgetItem,
                               QTreeWidget, QTreeWidgetItem, QVBoxLayout, QWidget)

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
APP_ID = "forza-linux-launcher"
GAME_EXE = "forza_durango_release_final.exe"
# Movies (media/ui/videos/*.bk2), in the order a new player sees them.
LOGO_MOVIE = "T10_MS_Combined"  # Turn 10 / Microsoft Studios logos
INTRO_MOVIE = "Anthem_Tone_Video"  # long pre-rendered movie after the logos
OPENING_MOVIE = "IE_opening"  # long opening movie of a new game (fresh save), ~1 min
LONG_MOVIES = [INTRO_MOVIE, OPENING_MOVIE]
AUTOPRESS_MENUS = "40:A,42:A,46:A,50:A,55:A,60:A"  # gets a test run past the title menus
DEV_BUILD_MARKER = b"FL_SKIP_MOVIES, developer build"  # only in -DFL_DEV_SKIP_MOVIES=ON builds
VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation"


def _xdg(var: str, default: str) -> Path:
    v = os.environ.get(var)
    return Path(v) if v else Path.home() / default


CONFIG_DIR = _xdg("XDG_CONFIG_HOME", ".config") / "forza-linux"
CONFIG_FILE = CONFIG_DIR / "launcher.json"
CACHE_DIR = _xdg("XDG_CACHE_HOME", ".cache") / "forza-linux"
LOG_DIR = CACHE_DIR / "logs"
DATA_HOME = _xdg("XDG_DATA_HOME", ".local/share")
KEEP_LOGS = 30
SELF = Path(__file__).resolve()
SELF_MTIME = SELF.stat().st_mtime  # Play restarts the launcher when its code changed since start

# ---------------------------------------------------------------- palette

C = dict(bg="#0c0e13", side="#10131a", card="#151922", card2="#1b2030", line="#242b3b", text="#e9ecf3",
         muted="#8b94a9", dim="#5f677c", pink="#ff2d78", orange="#ff8a00", green="#34d399", red="#ff4d5e",
         amber="#ffb020", blue="#5aa9ff")

# ---------------------------------------------------------------- settings

LOG_CHANNELS = [
    ("d3d", "Direct3D 11.X calls"), ("dma", "DMA engine copies and fills"),
    ("fence", "GPU/DMA fences, command lists, presents"), ("seh", "exceptions (SEH and C++)"),
    ("file", "file I/O and movies"), ("kernelx", "kernelx system calls"), ("thread", "threads"),
    ("concrt", "ConcRT / PPL tasks"), ("winrt", "WinRT activation"), ("audio", "audio, SHAPE, XMA"),
    ("memory", "memory allocation"), ("loader", "PE loader"), ("input", "controller input"),
    ("net", "networking"), ("shaders", "shader translation"), ("query", "GPU queries"),
    ("create", "resource creation"), ("remap", "bundle inheritance remaps"), ("platform", "window and platform"),
    ("all", "every channel (very verbose)"),
]

MH_LAYOUTS = [("fps", "FPS only"), ("horizontal", "Horizontal bar"), ("extended", "Extended"),
              ("detailed", "Detailed"), ("custom", "Custom")]
MH_METRICS = [("fps", "FPS"), ("frametime", "Frame time"), ("frame_timing", "Frame-time graph"),
              ("gpu_stats", "GPU load"), ("gpu_temp", "GPU temp"), ("gpu_core_clock", "GPU clock"),
              ("gpu_power", "GPU power"), ("vram", "VRAM"), ("cpu_stats", "CPU load"), ("cpu_temp", "CPU temp"),
              ("ram", "RAM"), ("gpu_name", "GPU name"), ("vulkan_driver", "Vulkan driver"),
              ("resolution", "Resolution"), ("gamemode", "GameMode"), ("time", "Clock")]
MH_PRESETS = {
    "horizontal": ["fps", "frametime", "gpu_stats", "cpu_stats", "vram", "ram"],
    "extended": ["fps", "frametime", "frame_timing", "gpu_stats", "gpu_temp", "gpu_core_clock", "cpu_stats",
                 "cpu_temp", "vram", "ram"],
    "detailed": ["fps", "frametime", "frame_timing", "gpu_stats", "gpu_temp", "gpu_core_clock", "gpu_power",
                 "cpu_stats", "cpu_temp", "vram", "ram", "gpu_name", "vulkan_driver", "resolution", "gamemode",
                 "time"],
}
MH_POSITIONS = [("top-left", "Top left"), ("top-center", "Top centre"), ("top-right", "Top right"),
                ("bottom-left", "Bottom left"), ("bottom-center", "Bottom centre"), ("bottom-right", "Bottom right")]
DXVK_HUD_ITEMS = [("fps", "FPS"), ("frametimes", "Frame times"), ("gpuload", "GPU load"), ("memory", "Memory"),
                  ("submissions", "Submissions"), ("drawcalls", "Draw calls"), ("pipelines", "Pipelines"),
                  ("compiler", "Shader compiler"), ("version", "DXVK version"), ("devinfo", "Device info")]
FRAME_PACING = [("", "Console timing: 30 fps"),
                ("dynamic", "Dynamic (not working yet: runs too fast)"),
                ("off", "Uncapped, game runs too fast")]
DISPLAY_MODES = [("windowed", "Windowed"), ("borderless", "Borderless window"), ("fullscreen", "Fullscreen")]
WINDOW_SIZES = [("", "1280 × 720 (runtime default)"), ("1600x900", "1600 × 900"), ("1920x1080", "1920 × 1080"),
                ("2560x1440", "2560 × 1440"), ("3840x2160", "3840 × 2160"), ("custom", "Custom…")]

DEFAULTS = {
    "game_dir": "", "runtime": "",
    "window": "", "window_custom": "1920x1080", "audio": True, "gamemode": False, "frame_pacing": "", "fps_cap": 0, "display_mode": "windowed",
    "skip_intro_movie": True, "skip_logo_movie": False, "skip_extra_movies": "",
    "minimize_on_play": False, "logs_on_crash": True, "warn_other_instance": True,
    "mangohud": False, "mh_layout": "extended", "mh_metrics": list(MH_PRESETS["extended"]),
    "mh_position": "top-left", "mh_fps_limit": 0, "mh_font_size": 22, "mh_alpha": 45, "mh_hidden": False,
    "mh_extra": "",
    "dxvk_hud": False, "dxvk_hud_items": ["fps", "frametimes", "gpuload"], "dxvk_log_level": "",
    "log_channels": [], "log_custom": "",
    "exit_after_frame": 0, "screenshot_frames": "", "autopress": "",
    "test_profile": False, "test_profile_dir": str(CACHE_DIR / "test-profile"),
    "vk_validation": False, "env": [], "extra_args": "",
    "page": 0, "last_session": None,
}


def load_config() -> dict:
    cfg = json.loads(json.dumps(DEFAULTS))
    try:
        cfg.update(json.loads(CONFIG_FILE.read_text()))
    except (OSError, ValueError):
        pass
    if cfg.pop("fullscreen", False) and cfg["display_mode"] == "windowed":
        cfg["display_mode"] = "borderless"  # the old fullscreen switch was borderless
    if cfg["display_mode"] not in {m for m, _ in DISPLAY_MODES}:
        cfg["display_mode"] = "windowed"
    if not cfg["game_dir"]:
        cfg["game_dir"] = discover_game_dir()
    if not cfg["runtime"]:
        cfg["runtime"] = discover_runtime()
    return cfg


def save_config(cfg: dict) -> None:
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    tmp = CONFIG_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(cfg, indent=2))
    tmp.replace(CONFIG_FILE)


# ---------------------------------------------------------------- environment probes

def discover_game_dir() -> str:
    # A dump extracted where tools/extract_dump.sh puts it; otherwise the user picks one.
    for exe in sorted(REPO.glob(f"game_data/*/Mount/{GAME_EXE}")):
        return str(exe.parent)
    return ""


def discover_runtime() -> str:
    cand = REPO / "build" / "forza-linux"
    return str(cand) if cand.is_file() else ""


@dataclass
class GameInfo:
    ok: bool
    reason: str = ""
    name: str = "Forza Horizon 2"
    version: str = ""
    splash: Path | None = None
    logo: Path | None = None


def game_info(d: str) -> GameInfo:
    if not d:
        return GameInfo(False, "No game folder chosen")
    p = Path(d)
    if not p.is_dir():
        return GameInfo(False, "Folder does not exist")
    if not (p / GAME_EXE).is_file():
        return GameInfo(False, f"{GAME_EXE} not found here (pick the dump's Mount folder)")
    gi = GameInfo(True)
    try:
        manifest = (p / "AppxManifest.xml").read_text(errors="replace")
        if m := re.search(r'<Identity\b[^>]*\bVersion="([^"]+)"', manifest):
            gi.version = m.group(1)
        if m := re.search(r"<DisplayName>([^<]+)</DisplayName>", manifest):
            gi.name = m.group(1).strip()
    except OSError:
        pass
    gi.splash = p / "SplashScreen.png" if (p / "SplashScreen.png").is_file() else None
    gi.logo = p / "WideLogo.png" if (p / "WideLogo.png").is_file() else None
    return gi


@dataclass
class RuntimeInfo:
    path: Path | None
    ok: bool = False
    reason: str = ""
    mtime: float = 0.0
    dev_build: bool = False
    build_dir: Path | None = None
    git: str = ""

    @staticmethod
    def probe(path: str) -> "RuntimeInfo":
        try:
            key = (path, os.stat(path).st_mtime) if path else None
        except OSError:
            key = None
        if key and key in _probe_cache:
            return _probe_cache[key]
        ri = RuntimeInfo._probe(path)
        if key:
            _probe_cache[key] = ri
        return ri

    @staticmethod
    def _probe(path: str) -> "RuntimeInfo":
        if not path:
            return RuntimeInfo(None, reason="No runtime binary chosen")
        p = Path(path)
        ri = RuntimeInfo(p)
        if not p.is_file():
            ri.reason = "Not built yet (run tools/build_dxvk.sh, then cmake + ninja)"
            return ri
        if not os.access(p, os.X_OK):
            ri.reason = "Not executable"
            return ri
        ri.ok = True
        ri.mtime = p.stat().st_mtime
        try:
            ri.dev_build = DEV_BUILD_MARKER in p.read_bytes()
        except OSError:
            pass
        if (p.parent / "build.ninja").is_file():
            ri.build_dir = p.parent
        ri.git = git_describe(p.parent)
        return ri


_probe_cache: dict = {}


def git_describe(where: Path) -> str:
    try:
        rev = subprocess.run(["git", "-C", str(where), "rev-parse", "--short", "HEAD"], capture_output=True,
                             text=True, timeout=3).stdout.strip()
        if not rev:
            return ""
        dirty = subprocess.run(["git", "-C", str(where), "status", "--porcelain", "--untracked-files=no"],
                               capture_output=True, text=True, timeout=3).stdout.strip()
        return rev + (" +local changes" if dirty else "")
    except (OSError, subprocess.SubprocessError):
        return ""


def running_instances(exclude: set[int] = frozenset()) -> list[int]:
    pids = []
    for d in Path("/proc").iterdir():
        if not d.name.isdigit() or int(d.name) in exclude:
            continue
        try:
            exe = os.readlink(d / "exe")
        except OSError:
            continue
        if re.fullmatch(r"forza-linux( \(deleted\))?", os.path.basename(exe)):
            pids.append(int(d.name))
    return pids


def have_validation_layer() -> bool:
    dirs = ["/usr/share/vulkan/explicit_layer.d", "/etc/vulkan/explicit_layer.d",
            str(DATA_HOME / "vulkan/explicit_layer.d")]
    return any(Path(d, "VkLayer_khronos_validation.json").is_file() for d in dirs)


def data_home(cfg: dict) -> Path:
    if cfg["test_profile"] and cfg["test_profile_dir"]:
        return Path(cfg["test_profile_dir"]).expanduser()
    return DATA_HOME


def save_root(cfg: dict) -> Path:
    return data_home(cfg) / "forza-linux"  # where the runtime keeps saves (XDG_DATA_HOME/forza-linux)


def backups_root(cfg: dict) -> Path:
    return data_home(cfg) / "forza-linux-backups"


def dir_size(p: Path) -> int:
    total = 0
    for root, _, files in os.walk(p):
        for f in files:
            try:
                total += os.lstat(os.path.join(root, f)).st_size
            except OSError:
                pass
    return total


def dir_mtime(p: Path) -> float:
    newest = 0.0
    for root, _, files in os.walk(p):
        for f in files:
            try:
                newest = max(newest, os.lstat(os.path.join(root, f)).st_mtime)
            except OSError:
                pass
    return newest


def human_size(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return ""


def human_duration(s: float) -> str:
    s = int(s)
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m {s % 60:02d}s"
    return f"{s // 3600}h {s % 3600 // 60:02d}m"


def human_ago(t: float) -> str:
    if not t:
        return "never"
    d = time.time() - t
    if d < 60:
        return "just now"
    if d < 3600:
        return f"{int(d // 60)} min ago"
    if d < 86400:
        return f"{int(d // 3600)} h ago"
    if d < 7 * 86400:
        return f"{int(d // 86400)} days ago"
    return time.strftime("%d %b %Y", time.localtime(t))


# ---------------------------------------------------------------- what Play runs

def mangohud_config(cfg: dict) -> str:
    layout = cfg["mh_layout"]
    opts: list[str] = []
    if layout == "fps":
        opts.append("fps_only")
    else:
        opts.append("legacy_layout=0")  # show only what we list
        if layout == "horizontal":
            opts.append("horizontal")
        opts += MH_PRESETS.get(layout, cfg["mh_metrics"])
    opts += [f"position={cfg['mh_position']}", f"font_size={cfg['mh_font_size']}",
             f"background_alpha={cfg['mh_alpha'] / 100:.2f}", "toggle_hud=Shift_R+F12"]
    if cfg["mh_fps_limit"] > 0:
        opts.append(f"fps_limit={cfg['mh_fps_limit']}")
        if layout != "fps":
            opts.append("show_fps_limit")
    if cfg["mh_hidden"]:
        opts.append("no_display")
    opts += [s.strip() for s in cfg["mh_extra"].split(",") if s.strip()]
    return ",".join(opts)


@dataclass
class LaunchPlan:
    cmd: list[str]
    env: dict[str, str]
    cwd: Path
    problems: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def shell(self, multiline: bool = True) -> str:
        env = [f"{k}={shlex.quote(v)}" for k, v in self.env.items()]
        if multiline:
            lines = [f"cd {shlex.quote(str(self.cwd))}"] + [e + " \\" for e in env] + [shlex.join(self.cmd)]
            return "\n".join(lines)
        return f"cd {shlex.quote(str(self.cwd))} && " + " ".join(env + [shlex.join(self.cmd)])


def build_launch(cfg: dict, rt: RuntimeInfo) -> LaunchPlan:
    env: dict[str, str] = {}
    problems, notes = [], []
    gi = game_info(cfg["game_dir"])
    if not gi.ok:
        problems.append(f"Game folder: {gi.reason}")
    if not rt.ok:
        problems.append(f"Runtime: {rt.reason}")

    if cfg["window"] == "custom":
        if re.fullmatch(r"\d{3,5}x\d{3,5}", cfg["window_custom"].strip()):
            env["FL_WINDOW"] = cfg["window_custom"].strip()
        else:
            problems.append("Window size: write a custom size as WIDTHxHEIGHT, e.g. 1920x1080")
    elif cfg["window"]:
        env["FL_WINDOW"] = cfg["window"]
    if not cfg["audio"]:
        env["FL_AUDIO"] = "null"
    if cfg["display_mode"] != "windowed":
        env["FL_DISPLAY_MODE"] = cfg["display_mode"]
    if cfg["frame_pacing"]:
        env["FL_FRAME_PACING"] = cfg["frame_pacing"]
    if cfg["frame_pacing"] == "dynamic" and cfg["fps_cap"] > 0:
        env["FL_FPS_CAP"] = str(cfg["fps_cap"])

    skips = (LONG_MOVIES if cfg["skip_intro_movie"] else []) + ([LOGO_MOVIE] if cfg["skip_logo_movie"] else [])
    skips += [s.strip() for s in cfg["skip_extra_movies"].split(",") if s.strip()]
    if skips:
        if rt.dev_build:
            env["FL_SKIP_MOVIES"] = ",".join(skips)
        elif rt.ok:
            notes.append("Movie skipping is off: this runtime is a release build (needs -DFL_DEV_SKIP_MOVIES=ON)")

    channels = list(cfg["log_channels"]) + [c.strip() for c in cfg["log_custom"].split(",") if c.strip()]
    if channels:
        env["FL_LOG"] = "all" if "all" in channels else ",".join(dict.fromkeys(channels))
    if cfg["exit_after_frame"] > 0:
        env["FL_EXIT_AFTER_FRAME"] = str(cfg["exit_after_frame"])
    if cfg["screenshot_frames"].strip():
        env["FL_SCREENSHOT_AT_FRAME"] = re.sub(r"\s+", "", cfg["screenshot_frames"])
    if cfg["autopress"].strip():
        env["FL_AUTOPRESS"] = re.sub(r"\s+", "", cfg["autopress"])
    if cfg["test_profile"]:
        env["XDG_DATA_HOME"] = str(data_home(cfg))

    if cfg["dxvk_hud"] and cfg["dxvk_hud_items"]:
        env["DXVK_HUD"] = ",".join(cfg["dxvk_hud_items"])
    if cfg["dxvk_log_level"]:
        env["DXVK_LOG_LEVEL"] = cfg["dxvk_log_level"]
    if cfg["vk_validation"]:
        env["VK_INSTANCE_LAYERS"] = VALIDATION_LAYER

    prefix = []
    if cfg["gamemode"]:
        if shutil.which("gamemoderun"):
            prefix.append("gamemoderun")
        else:
            notes.append("GameMode is not installed (sudo dnf install gamemode); launching without it")
    if cfg["mangohud"]:
        if shutil.which("mangohud"):
            prefix.append("mangohud")
            env["MANGOHUD_CONFIG"] = mangohud_config(cfg)
        else:
            notes.append("MangoHud is not installed (sudo dnf install mangohud); launching without it")

    for row in cfg["env"]:
        name = row.get("name", "").strip()
        if row.get("on", True) and name:
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
                env[name] = row.get("value", "")
            else:
                problems.append(f"Environment: '{name}' is not a valid variable name")
    try:
        extra = shlex.split(cfg["extra_args"])
    except ValueError as e:
        extra = []
        problems.append(f"Extra arguments: {e}")

    rt_path = rt.path or Path("forza-linux")
    # The runtime writes screenshots/ relative to its working directory: use the checkout.
    cwd = rt_path.parent.parent if rt_path.parent.name.startswith("build") else rt_path.parent
    cmd = prefix + [str(rt_path), "--game-dir", cfg["game_dir"]] + extra
    return LaunchPlan(cmd, env, cwd, problems, notes)


# ---------------------------------------------------------------- shortcuts

def desktop_quote(s: str) -> str:
    # Desktop Entry spec, "The Exec key": quote arguments with reserved characters.
    s = s.replace("%", "%%")
    if re.search(r"[\s\"'\\><~|&;$*?#()`]", s):
        return '"' + s.replace("\\", "\\\\\\\\").replace('"', '\\"').replace("`", "\\`").replace("$", "\\$") + '"'
    return s


def make_game_icon(cfg: dict, dest: Path) -> bool:
    # The desktop icon: the game's own wide logo from the user's game folder,
    # rounded. Written only to the user's icon folder, never to the repo.
    gi = game_info(cfg["game_dir"])
    if not gi.logo:
        return False
    img = QImage(str(gi.logo))
    if img.isNull():
        return False
    out = QImage(256, 256, QImage.Format.Format_ARGB32_Premultiplied)
    out.fill(Qt.GlobalColor.transparent)
    p = QPainter(out)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
    tile = QRectF(6, 6, 244, 244)
    path = QPainterPath()
    path.addRoundedRect(tile, 46, 46)
    p.setClipPath(path)
    p.drawImage(tile, img)
    p.setClipping(False)
    p.setPen(QPen(QColor(255, 255, 255, 48), 2))
    p.setBrush(Qt.BrushStyle.NoBrush)
    p.drawPath(path)
    p.end()
    dest.parent.mkdir(parents=True, exist_ok=True)
    return out.save(str(dest))


def icon_path(cfg: dict) -> Path:
    png = DATA_HOME / "icons/hicolor/256x256/apps" / f"{APP_ID}.png"
    return png if png.is_file() else HERE / "forza-linux.svg"


def install_shortcuts(cfg: dict) -> list[str]:
    done = []
    png = DATA_HOME / "icons/hicolor/256x256/apps" / f"{APP_ID}.png"
    if make_game_icon(cfg, png):
        icon = png
        done.append(f"icon from your game's logo: {png}")
    else:
        icon = HERE / "forza-linux.svg"
        done.append(f"icon: {icon} (no game folder set, so no game logo)")
    entry = "\n".join([
        "[Desktop Entry]",
        "Type=Application",
        "Version=1.5",
        "Name=Forza Horizon 2",
        "GenericName=Racing game (forza-linux)",
        "Comment=Play Forza Horizon 2 with the forza-linux runtime",
        f"Exec={desktop_quote(sys.executable)} {desktop_quote(str(HERE / 'forza_launcher.py'))}",
        f"Path={HERE}",
        f"Icon={icon}",
        "Terminal=false",
        "Categories=Game;",
        "Keywords=forza;horizon;racing;xbox;",
        "StartupNotify=true",
        f"StartupWMClass={APP_ID}",
        "",
    ])
    apps = DATA_HOME / "applications"
    apps.mkdir(parents=True, exist_ok=True)
    targets = [apps / f"{APP_ID}.desktop"]
    try:
        desk = subprocess.run(["xdg-user-dir", "DESKTOP"], capture_output=True, text=True,
                              timeout=3).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        desk = ""
    desktop_dir = Path(desk) if desk and desk != str(Path.home()) else Path.home() / "Desktop"
    if desktop_dir.is_dir():
        targets.append(desktop_dir / f"{APP_ID}.desktop")
    for t in targets:
        t.write_text(entry)
        t.chmod(0o755)  # Plasma only trusts executable desktop files
        done.append(f"shortcut: {t}")
    for tool in (["update-desktop-database", str(apps)], ["kbuildsycoca6"]):
        if shutil.which(tool[0]):
            subprocess.run(tool, capture_output=True, timeout=60)
    return done


# ---------------------------------------------------------------- widgets

class Toggle(QAbstractButton):
    """An animated on/off switch."""

    def __init__(self, checked: bool = False, parent: QWidget | None = None):
        super().__init__(parent)
        self.setCheckable(True)
        self.setChecked(checked)
        self._t = 1.0 if checked else 0.0
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFixedSize(46, 26)
        self._anim = QPropertyAnimation(self, b"knob", self)
        self._anim.setDuration(170)
        self._anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self.toggled.connect(self._animate)

    def _animate(self, on: bool) -> None:
        self._anim.stop()
        self._anim.setStartValue(self._t)
        self._anim.setEndValue(1.0 if on else 0.0)
        self._anim.start()

    def set_instant(self, on: bool) -> None:
        self.blockSignals(True)
        self.setChecked(on)
        self.blockSignals(False)
        self._anim.stop()
        self._t = 1.0 if on else 0.0
        self.update()

    def _get(self) -> float:
        return self._t

    def _set(self, v: float) -> None:
        self._t = v
        self.update()

    knob = Property(float, _get, _set)

    def sizeHint(self) -> QSize:
        return QSize(46, 26)

    def paintEvent(self, _) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        rad = r.height() / 2
        if not self.isEnabled():
            p.setOpacity(0.35)
        p.setPen(QPen(QColor(C["line"]), 1))
        p.setBrush(QColor("#222838"))
        p.drawRoundedRect(r, rad, rad)
        if self._t > 0:
            g = QLinearGradient(r.topLeft(), r.topRight())
            g.setColorAt(0, QColor(C["pink"]))
            g.setColorAt(1, QColor(C["orange"]))
            p.setOpacity(p.opacity() * self._t)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QBrush(g))
            p.drawRoundedRect(r, rad, rad)
            p.setOpacity(0.35 if not self.isEnabled() else 1.0)
        d = r.height() - 6
        x = r.left() + 3 + self._t * (r.width() - d - 6)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(0, 0, 0, 60))
        p.drawEllipse(QRectF(x, r.top() + 4, d, d))
        p.setBrush(QColor("#ffffff"))
        p.drawEllipse(QRectF(x, r.top() + 3, d, d))


class Card(QFrame):
    def __init__(self, title: str = "", subtitle: str = "", parent: QWidget | None = None):
        super().__init__(parent)
        self.setObjectName("card")
        self.lay = QVBoxLayout(self)
        self.lay.setContentsMargins(22, 18, 22, 14)
        self.lay.setSpacing(2)
        self._rows = 0
        if title:
            t = QLabel(title)
            t.setObjectName("cardTitle")
            self.lay.addWidget(t)
        if subtitle:
            s = QLabel(subtitle)
            s.setObjectName("muted")
            s.setWordWrap(True)
            self.lay.addWidget(s)
        if title or subtitle:
            self.lay.addSpacing(6)

    def add_row(self, title: str, desc: str, control: QWidget, wide: bool = False) -> "SettingRow":
        line = None
        if self._rows:
            line = QFrame()
            line.setObjectName("sep")
            line.setFixedHeight(1)
            self.lay.addWidget(line)
        row = SettingRow(title, desc, control, wide)
        row.sep = line
        self.lay.addWidget(row)
        self._rows += 1
        return row

    def add(self, w: QWidget) -> QWidget:
        self.lay.addWidget(w)
        return w


class SettingRow(QWidget):
    def __init__(self, title: str, desc: str, control: QWidget, wide: bool = False):
        super().__init__()
        h = QHBoxLayout(self)
        h.setContentsMargins(0, 10, 0, 10)
        h.setSpacing(18)
        v = QVBoxLayout()
        v.setSpacing(3)
        self.title = QLabel(title)
        self.title.setObjectName("rowTitle")
        v.addWidget(self.title)
        self.desc = QLabel(desc)
        self.desc.setObjectName("muted")
        self.desc.setWordWrap(True)
        self.desc.setVisible(bool(desc))
        v.addWidget(self.desc)
        h.addLayout(v, 2 if wide else 1)
        if wide:
            h.addWidget(control, 3)
        else:
            h.addWidget(control, 0, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight)

    def set_desc(self, text: str) -> None:
        self.desc.setText(text)
        self.desc.setVisible(bool(text))

    def show_row(self, on: bool) -> None:
        self.setVisible(on)
        if self.sep:
            self.sep.setVisible(on)


class Pill(QLabel):
    def set(self, text: str, kind: str) -> None:
        self.setText(f"●  {text}")
        self.setProperty("kind", kind)
        self.style().unpolish(self)
        self.style().polish(self)


class InfoCard(QFrame):
    clicked = Signal()

    def __init__(self, title: str):
        super().__init__()
        self.setObjectName("info")
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        v = QVBoxLayout(self)
        v.setContentsMargins(18, 14, 18, 14)
        v.setSpacing(3)
        t = QLabel(title.upper())
        t.setObjectName("infoTitle")
        self.value = QLabel("–")
        self.value.setObjectName("infoValue")
        self.detail = QLabel("")
        self.detail.setObjectName("muted")
        self.detail.setWordWrap(True)
        v.addWidget(t)
        v.addWidget(self.value)
        v.addWidget(self.detail)
        v.addStretch(1)

    def set(self, value: str, detail: str = "", kind: str = "") -> None:
        self.value.setText(value)
        self.detail.setText(detail)
        self.value.setProperty("kind", kind)
        self.value.style().unpolish(self.value)
        self.value.style().polish(self.value)

    def mousePressEvent(self, e) -> None:
        if e.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit()


def paint_sunset(p: QPainter, r: QRectF) -> None:
    # Fallback hero art (no game folder yet): the launcher icon's sunset road.
    g = QLinearGradient(r.topLeft(), r.bottomLeft())
    for stop, col in ((0, "#2a0c45"), (0.5, "#a0185b"), (0.78, "#ff5a3c"), (1, "#ff9e1b")):
        g.setColorAt(stop, QColor(col))
    p.fillRect(r, g)
    sun = QRadialGradient(r.center().x() + r.width() * 0.18, r.top() + r.height() * 0.62, r.height() * 0.32)
    sun.setColorAt(0, QColor("#fff3b0"))
    sun.setColorAt(0.7, QColor("#ffb02e"))
    sun.setColorAt(1, QColor(255, 176, 46, 0))
    p.fillRect(r, sun)
    hz = r.top() + r.height() * 0.72
    hill = QPainterPath()
    hill.moveTo(r.left(), hz)
    hill.cubicTo(r.left() + r.width() * 0.3, hz - 30, r.left() + r.width() * 0.6, hz + 20, r.right(), hz - 10)
    hill.lineTo(r.right(), r.bottom())
    hill.lineTo(r.left(), r.bottom())
    p.fillPath(hill, QColor("#1c0b24"))
    cx = r.center().x() + r.width() * 0.18
    road = QPainterPath()
    road.moveTo(cx - 14, hz)
    road.lineTo(cx + 14, hz)
    road.lineTo(cx + r.width() * 0.32, r.bottom())
    road.lineTo(cx - r.width() * 0.32, r.bottom())
    road.closeSubpath()
    p.fillPath(road, QColor("#2b2136"))


class Hero(QWidget):
    """The Play page banner: the game's splash art, darkened where text sits."""

    def __init__(self):
        super().__init__()
        self._pix: QPixmap | None = None
        self._scaled: QPixmap | None = None
        self.setMinimumHeight(330)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

    def set_art(self, path: Path | None) -> None:
        if str(path) == getattr(self, "_path", None):
            return
        self._path = str(path)
        pix = QPixmap(str(path)) if path else QPixmap()
        self._pix = None if pix.isNull() else pix
        self._scaled = None
        self.update()

    def has_art(self) -> bool:
        return self._pix is not None

    def resizeEvent(self, e) -> None:
        self._scaled = None
        super().resizeEvent(e)

    def paintEvent(self, _) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        r = QRectF(self.rect())
        clip = QPainterPath()
        clip.addRoundedRect(r, 20, 20)
        p.setClipPath(clip)
        if self._pix:
            if self._scaled is None or self._scaled.size() != self.size():
                s = self._pix.scaled(self.size(), Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                                     Qt.TransformationMode.SmoothTransformation)
                x = (s.width() - self.width()) // 2
                y = int((s.height() - self.height()) * 0.42)
                self._scaled = s.copy(x, y, self.width(), self.height())
            p.drawPixmap(0, 0, self._scaled)
        else:
            paint_sunset(p, r)
        side = QLinearGradient(r.topLeft(), r.topRight())
        side.setColorAt(0, QColor(8, 9, 14, 236))
        side.setColorAt(0.38, QColor(8, 9, 14, 170))
        side.setColorAt(0.7, QColor(8, 9, 14, 0))
        p.fillRect(r, side)
        bottom = QLinearGradient(0, r.height() * 0.45, 0, r.height())
        bottom.setColorAt(0, QColor(12, 14, 19, 0))
        bottom.setColorAt(1, QColor(12, 14, 19, 215))
        p.fillRect(r, bottom)
        p.setClipping(False)
        p.setPen(QPen(QColor(255, 255, 255, 26), 1))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 20, 20)


class LogHighlighter(QSyntaxHighlighter):
    PREFIX = re.compile(r"^\[ *[0-9.]+ +\d+ +[\w ]+\]")

    def __init__(self, doc):
        super().__init__(doc)

        def fmt(color: str, bold: bool = False) -> QTextCharFormat:
            f = QTextCharFormat()
            f.setForeground(QColor(color))
            if bold:
                f.setFontWeight(QFont.Weight.Bold)
            return f

        self.prefix = fmt(C["dim"])
        self.rules = [
            (re.compile(r"\b(ERROR|FATAL|UNHANDLED|UNIMPLEMENTED)\b|^err:.*"), fmt(C["red"], True)),
            (re.compile(r"\bSTUB\b|^warn:.*"), fmt(C["amber"])),
            (re.compile(r"^\[launcher\].*"), fmt(C["pink"], True)),
            (re.compile(r"^\[build\].*"), fmt(C["blue"])),
        ]

    def highlightBlock(self, text: str) -> None:
        if m := self.PREFIX.match(text):
            self.setFormat(0, m.end(), self.prefix)
        for rx, f in self.rules:
            for m in rx.finditer(text):
                self.setFormat(m.start(), m.end() - m.start(), f)


ERROR_RX = re.compile(r"\b(ERROR|FATAL|UNHANDLED|UNIMPLEMENTED|STUB)\b|^err:")


class Console(QPlainTextEdit):
    """The game's live output. Lines are buffered and drawn 10 times a second
    so a chatty FL_LOG run can't stall the launcher (or, through the pipe, the
    game); the full output is always in the session log file."""

    MAX_SHOWN = 20000

    def __init__(self):
        super().__init__()
        self.setReadOnly(True)
        self.setMaximumBlockCount(self.MAX_SHOWN)
        self.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        f = QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
        f.setPointSizeF(max(8.5, f.pointSizeF() - 1))
        self.setFont(f)
        self.setObjectName("console")
        self.lines: collections.deque[str] = collections.deque(maxlen=200000)
        self.pending: list[str] = []
        self.partial = ""
        self.filter = ""
        self.errors_only = False
        self.follow = True
        LogHighlighter(self.document())
        self.timer = QTimer(self)
        self.timer.setInterval(100)
        self.timer.timeout.connect(self.flush)
        self.timer.start()

    def match(self, line: str) -> bool:
        if self.errors_only and not ERROR_RX.search(line):
            return False
        return not self.filter or self.filter in line.lower()

    def feed(self, text: str) -> None:
        parts = (self.partial + text).split("\n")
        self.partial = parts.pop()
        for line in parts:
            line = line.rstrip("\r")
            self.lines.append(line)
            if self.match(line):
                self.pending.append(line)

    def note(self, msg: str) -> None:
        self.feed(f"[launcher] {msg}\n")

    def flush(self) -> None:
        if not self.pending:
            return
        lines, self.pending = self.pending, []
        if len(lines) > 4000:
            lines = [f"[launcher] … {len(lines) - 4000} lines not shown (all of them are in the session log)"] + \
                    lines[-4000:]
        sb = self.verticalScrollBar()
        at_end = sb.value() >= sb.maximum() - 4
        self.appendPlainText("\n".join(lines))
        if self.follow and at_end:
            sb.setValue(sb.maximum())

    def refilter(self) -> None:
        self.clear()
        self.pending = [l for l in self.lines if self.match(l)][-self.MAX_SHOWN:]
        self.flush()
        self.verticalScrollBar().setValue(self.verticalScrollBar().maximum())

    def wipe(self) -> None:
        self.lines.clear()
        self.pending = []
        self.clear()


# ---------------------------------------------------------------- settings binding

class Binder(QObject):
    """Ties widgets to config keys; widgets sharing a key stay in sync."""

    changed = Signal(str)

    def __init__(self, cfg: dict):
        super().__init__()
        self.cfg = cfg
        self.views: dict[str, list] = collections.defaultdict(list)
        self.saver = QTimer(self)
        self.saver.setSingleShot(True)
        self.saver.setInterval(400)
        self.saver.timeout.connect(lambda: save_config(self.cfg))

    def set(self, key: str, value, source: QWidget | None = None) -> None:
        if self.cfg.get(key) == value:
            return
        self.cfg[key] = value
        self.saver.start()
        for w, show in self.views[key]:
            if w is not source:
                show(value)
        self.changed.emit(key)

    def refresh(self, key: str) -> None:
        for _, show in self.views[key]:
            show(self.cfg[key])

    def toggle(self, key: str) -> Toggle:
        w = Toggle(bool(self.cfg[key]))
        w.toggled.connect(lambda v: self.set(key, bool(v), w))
        self.views[key].append((w, lambda v: w.set_instant(bool(v))))
        return w

    def line(self, key: str, placeholder: str = "", width: int = 0) -> QLineEdit:
        w = QLineEdit(str(self.cfg[key]))
        w.setPlaceholderText(placeholder)
        if width:
            w.setMinimumWidth(width)
        w.textChanged.connect(lambda v: self.set(key, v, w))

        def show(v):
            if w.text() != v:
                w.setText(v)
        self.views[key].append((w, show))
        return w

    def spin(self, key: str, lo: int, hi: int, suffix: str = "", special: str = "", step: int = 1) -> QSpinBox:
        w = QSpinBox()
        w.setRange(lo, hi)
        w.setSingleStep(step)
        w.setSuffix(suffix)
        if special:
            w.setSpecialValueText(special)
        w.setValue(int(self.cfg[key]))
        w.setMinimumWidth(130)
        w.valueChanged.connect(lambda v: self.set(key, int(v), w))

        def show(v):
            w.blockSignals(True)
            w.setValue(int(v))
            w.blockSignals(False)
        self.views[key].append((w, show))
        return w

    def combo(self, key: str, items: list[tuple[str, str]], width: int = 220) -> QComboBox:
        w = QComboBox()
        for value, label in items:
            w.addItem(label, value)
        w.setMinimumWidth(width)

        def show(v):
            w.blockSignals(True)
            i = w.findData(v)
            w.setCurrentIndex(i if i >= 0 else 0)
            w.blockSignals(False)
        show(self.cfg[key])
        w.currentIndexChanged.connect(lambda _: self.set(key, w.currentData(), w))
        self.views[key].append((w, show))
        return w

    def member(self, key: str, value: str, label: str, tip: str = "") -> QCheckBox:
        """A checkbox for whether `value` is in the list cfg[key]."""
        w = QCheckBox(label)
        w.setToolTip(tip)
        w.setChecked(value in self.cfg[key])

        def flip(on):
            cur = [x for x in self.cfg[key] if x != value]
            self.set(key, cur + [value] if on else cur, w)
        w.toggled.connect(flip)

        def show(v):
            w.blockSignals(True)
            w.setChecked(value in v)
            w.blockSignals(False)
        self.views[key].append((w, show))
        return w

    def slider(self, key: str, lo: int, hi: int, suffix: str = "") -> QWidget:
        box = QWidget()
        h = QHBoxLayout(box)
        h.setContentsMargins(0, 0, 0, 0)
        s = QSlider(Qt.Orientation.Horizontal)
        s.setRange(lo, hi)
        s.setValue(int(self.cfg[key]))
        s.setMinimumWidth(170)
        lab = QLabel(f"{self.cfg[key]}{suffix}")
        lab.setObjectName("muted")
        lab.setMinimumWidth(42)
        s.valueChanged.connect(lambda v: (lab.setText(f"{v}{suffix}"), self.set(key, int(v), s)))

        def show(v):
            s.blockSignals(True)
            s.setValue(int(v))
            s.blockSignals(False)
            lab.setText(f"{v}{suffix}")
        self.views[key].append((s, show))
        h.addWidget(s)
        h.addWidget(lab)
        return box


# ---------------------------------------------------------------- pages

def page_header(title: str, subtitle: str) -> QWidget:
    w = QWidget()
    v = QVBoxLayout(w)
    v.setContentsMargins(0, 0, 0, 8)
    v.setSpacing(2)
    t = QLabel(title)
    t.setObjectName("h1")
    s = QLabel(subtitle)
    s.setObjectName("muted")
    s.setWordWrap(True)
    v.addWidget(t)
    v.addWidget(s)
    return w


def scroll_page(title: str, subtitle: str) -> tuple[QScrollArea, QVBoxLayout]:
    inner = QWidget()
    inner.setObjectName("pageInner")
    v = QVBoxLayout(inner)
    v.setContentsMargins(34, 28, 34, 28)
    v.setSpacing(16)
    v.addWidget(page_header(title, subtitle))
    sa = QScrollArea()
    sa.setWidgetResizable(True)
    sa.setFrameShape(QFrame.Shape.NoFrame)
    sa.setWidget(inner)
    return sa, v


def button(text: str, icon: str = "", kind: str = "", tip: str = "") -> QPushButton:
    b = QPushButton(text)
    if icon:
        b.setIcon(QIcon.fromTheme(icon))
    if kind:
        b.setObjectName(kind)
    if tip:
        b.setToolTip(tip)
    b.setCursor(Qt.CursorShape.PointingHandCursor)
    return b


def path_field(binder: Binder, key: str, pick_dir: bool, caption: str) -> tuple[QWidget, QLineEdit]:
    box = QWidget()
    h = QHBoxLayout(box)
    h.setContentsMargins(0, 0, 0, 0)
    h.setSpacing(8)
    edit = binder.line(key)
    b = button("Browse…", "document-open-folder")

    def pick():
        start = edit.text() or str(Path.home())
        if pick_dir:
            p = QFileDialog.getExistingDirectory(box, caption, start)
        else:
            p, _ = QFileDialog.getOpenFileName(box, caption, start)
        if p:
            edit.setText(p)
    b.clicked.connect(pick)
    h.addWidget(edit, 1)
    h.addWidget(b)
    return box, edit


class PlayPage(QWidget):
    def __init__(self, win: "MainWindow"):
        super().__init__()
        self.win = win
        b = win.binder
        v = QVBoxLayout(self)
        v.setContentsMargins(34, 28, 34, 28)
        v.setSpacing(18)

        self.hero = Hero()
        hv = QVBoxLayout(self.hero)
        hv.setContentsMargins(40, 34, 40, 34)
        hv.setSpacing(10)
        self.title = QLabel("FORZA HORIZON 2")
        self.title.setObjectName("heroTitle")
        hv.addWidget(self.title)
        hv.addStretch(1)
        self.tagline = QLabel("Xbox One edition · running natively on Linux")
        self.tagline.setObjectName("heroTag")
        hv.addWidget(self.tagline)
        self.pill = Pill()
        self.pill.setObjectName("pill")
        pill_row = QHBoxLayout()
        pill_row.addWidget(self.pill)
        pill_row.addStretch(1)
        hv.addLayout(pill_row)
        hv.addSpacing(8)
        row = QHBoxLayout()
        row.setSpacing(12)
        self.play = QPushButton("PLAY")
        self.play.setObjectName("play")
        f = self.play.font()
        f.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, 3)
        self.play.setFont(f)
        self.play.setCursor(Qt.CursorShape.PointingHandCursor)
        self.play.clicked.connect(win.play_or_stop)
        row.addWidget(self.play)
        self.summary = QLabel("")
        self.summary.setObjectName("heroNote")
        self.summary.setWordWrap(True)
        row.addWidget(self.summary, 1, Qt.AlignmentFlag.AlignVCenter)
        hv.addLayout(row)
        v.addWidget(self.hero, 3)

        self.banner = QLabel("")
        self.banner.setObjectName("banner")
        self.banner.setWordWrap(True)
        self.banner.hide()
        v.addWidget(self.banner)

        quick = QHBoxLayout()
        quick.setSpacing(14)
        self.skip_chip = self._chip(quick, "Skip long movies", "Intro and opening", b.toggle("skip_intro_movie"))
        self._chip(quick, "MangoHud", "Performance overlay", b.toggle("mangohud"))
        self._chip(quick, "GameMode", "CPU boost", b.toggle("gamemode"))
        self.fps_combo = b.combo("frame_pacing", [("", "30 fps"), ("dynamic", "Dynamic (WIP)"), ("off", "Uncapped")],
                                 130)
        self._chip(quick, "Frame rate", "30 fps is the stable one", self.fps_combo)
        self._chip(quick, "Sound", "Game audio", b.toggle("audio"))
        v.addLayout(quick)

        info = QHBoxLayout()
        info.setSpacing(14)
        self.c_game = InfoCard("Game")
        self.c_runtime = InfoCard("Runtime")
        self.c_save = InfoCard("Save data")
        self.c_last = InfoCard("Last session")
        self.c_game.clicked.connect(lambda: win.go(1))
        self.c_runtime.clicked.connect(lambda: win.go(2))
        self.c_save.clicked.connect(lambda: win.go(3))
        self.c_last.clicked.connect(lambda: win.go(4))
        for c in (self.c_game, self.c_runtime, self.c_save, self.c_last):
            c.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Maximum)
            info.addWidget(c, 1)
        v.addLayout(info)

    def _chip(self, row: QHBoxLayout, title: str, desc: str, toggle: Toggle) -> QFrame:
        f = QFrame()
        f.setObjectName("chip")
        h = QHBoxLayout(f)
        h.setContentsMargins(16, 12, 14, 12)
        tv = QVBoxLayout()
        tv.setSpacing(1)
        t = QLabel(title)
        t.setObjectName("rowTitle")
        d = QLabel(desc)
        d.setObjectName("muted")
        tv.addWidget(t)
        tv.addWidget(d)
        h.addLayout(tv, 1)
        h.addWidget(toggle, 0, Qt.AlignmentFlag.AlignVCenter)
        f.toggle = toggle
        f.desc = d
        row.addWidget(f, 1)
        return f


class SettingsPage(QScrollArea):
    def __init__(self, win: "MainWindow"):
        super().__init__()
        self.win = win
        b = win.binder
        sa, v = scroll_page("Settings", "Where the game and the runtime live, and how the game starts.")
        self.setWidgetResizable(True)
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setWidget(sa.takeWidget())

        game = Card("Game files", "Your own dump of the Xbox One game: the Mount folder that holds "
                                  f"{GAME_EXE}. Nothing from it is ever copied into the project.")
        box, _ = path_field(b, "game_dir", True, "Choose the game's Mount folder")
        self.game_row = game.add_row("Game folder", "", box, wide=True)
        box, _ = path_field(b, "runtime", False, "Choose the forza-linux binary")
        self.rt_row = game.add_row("Runtime", "", box, wide=True)
        v.addWidget(game)

        disp = Card("Display and sound")
        disp.add_row("Display mode", "Windowed, a borderless window covering the screen, or exclusive fullscreen. "
                     "F11 or Alt+Enter switches between windowed and the other one while playing. Smoothest with "
                     "variable refresh on, or a 60/120/240 Hz display mode (30 fps divides evenly).",
                     b.combo("display_mode", DISPLAY_MODES))
        size = b.combo("window", WINDOW_SIZES)
        disp.add_row("Window size", "The game window starts at this size; you can resize it while playing. "
                     "In fullscreen, a size picks the screen resolution (the default keeps your desktop's).", size)
        disp.add_row("Frame rate", "The game was made for 30 fps. Dynamic lets it run faster while game time "
                     "follows the real clock; uncapped runs the game too fast.",
                     b.combo("frame_pacing", FRAME_PACING, 340))
        self.cap_row = disp.add_row("FPS cap", "Dynamic mode only. Off: up to your monitor's refresh rate.",
                                    b.spin("fps_cap", 0, 500, " fps", "Off", 10))
        self.custom = b.line("window_custom", "1920x1080", 140)
        self.custom_row = disp.add_row("Custom size", "WIDTHxHEIGHT", self.custom)
        disp.add_row("Sound", "Off runs the game with silent audio output.", b.toggle("audio"))
        v.addWidget(disp)

        perf = Card("Performance")
        gm = b.toggle("gamemode")
        has_gm = shutil.which("gamemoderun") is not None
        gm.setEnabled(has_gm)
        perf.add_row("GameMode", "Feral GameMode: performance CPU governor and higher priority while playing."
                     + ("" if has_gm else " Not installed: sudo dnf install gamemode"), gm)
        perf.add_row("MangoHud", "Performance overlay (configure it under Developer).", b.toggle("mangohud"))
        v.addWidget(perf)

        movies = Card("Movies")
        self.skip_row = movies.add_row(
            "Skip the long movies",
            f"Skips {INTRO_MOVIE} (after the studio logos) and {OPENING_MOVIE} (the opening of a new game).",
            b.toggle("skip_intro_movie"))
        v.addWidget(movies)

        launcher = Card("Launcher")
        launcher.add_row("Minimise while playing", "Hide the launcher while the game runs; it comes back "
                         "when the game exits.", b.toggle("minimize_on_play"))
        launcher.add_row("Show the log after a crash", "Jump to Logs when the game crashes.",
                         b.toggle("logs_on_crash"))
        launcher.add_row("Warn about a second copy", "Ask before starting while another forza-linux is "
                         "running: both share the GPU and each runs slower.", b.toggle("warn_other_instance"))
        sc = button("Create shortcuts", "list-add")
        sc.clicked.connect(win.install_shortcuts)
        launcher.add_row("Desktop shortcut", "Adds Forza Horizon 2 to the app menu and the desktop, with the "
                         "game's logo from your game folder.", sc)
        v.addWidget(launcher)
        v.addStretch(1)

        b.changed.connect(self.update_state)
        self.update_state()

    def update_state(self, key: str = "") -> None:
        self.custom_row.show_row(self.win.cfg["window"] == "custom")
        self.cap_row.show_row(self.win.cfg["frame_pacing"] == "dynamic")


class DevPage(QScrollArea):
    def __init__(self, win: "MainWindow"):
        super().__init__()
        self.win = win
        b = win.binder
        sa, v = scroll_page("Developer", "Overlays, logging, test automation and the exact command Play runs.")
        self.setWidgetResizable(True)
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setWidget(sa.takeWidget())

        # MangoHud
        has_mh = shutil.which("mangohud") is not None
        mh = Card("MangoHud", "Vulkan overlay for FPS, frame times and GPU/CPU load. Toggle it in game with "
                              "Right Shift + F12." + ("" if has_mh else "  Not installed: sudo dnf install mangohud"))
        t = b.toggle("mangohud")
        t.setEnabled(has_mh)
        mh.add_row("Enable MangoHud", "", t)
        mh.add_row("Layout", "", b.combo("mh_layout", MH_LAYOUTS))
        grid_box = QWidget()
        grid = QGridLayout(grid_box)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setHorizontalSpacing(18)
        grid.setVerticalSpacing(6)
        self.mh_checks = []
        for i, (key, label) in enumerate(MH_METRICS):
            cb = b.member("mh_metrics", key, label)
            self.mh_checks.append(cb)
            grid.addWidget(cb, i // 4, i % 4)
        self.mh_custom_row = mh.add_row("Custom items", "Used by the Custom layout.", grid_box, wide=True)
        mh.add_row("Position", "", b.combo("mh_position", MH_POSITIONS))
        mh.add_row("FPS limit", "MangoHud's frame limiter.", b.spin("mh_fps_limit", 0, 500, " fps", "Unlimited", 5))
        mh.add_row("Font size", "", b.spin("mh_font_size", 10, 64, " px"))
        mh.add_row("Background opacity", "", b.slider("mh_alpha", 0, 100, "%"))
        mh.add_row("Start hidden", "Load the overlay but keep it hidden until Right Shift + F12.",
                   b.toggle("mh_hidden"))
        mh.add_row("Extra options", "More MANGOHUD_CONFIG options, comma separated.",
                   b.line("mh_extra", "e.g. gpu_load_change,cpu_load_change"), wide=True)
        v.addWidget(mh)

        # DXVK
        dx = Card("DXVK", "The D3D11 → Vulkan layer the runtime draws with.")
        dx.add_row("DXVK HUD", "DXVK's own overlay.", b.toggle("dxvk_hud"))
        dgrid_box = QWidget()
        dgrid = QGridLayout(dgrid_box)
        dgrid.setContentsMargins(0, 0, 0, 0)
        dgrid.setHorizontalSpacing(18)
        dgrid.setVerticalSpacing(6)
        for i, (key, label) in enumerate(DXVK_HUD_ITEMS):
            dgrid.addWidget(b.member("dxvk_hud_items", key, label), i // 4, i % 4)
        dx.add_row("HUD items", "", dgrid_box, wide=True)
        dx.add_row("DXVK log level", "", b.combo("dxvk_log_level", [("", "Default (info)"), ("none", "None"),
                                                                    ("error", "Errors"), ("warn", "Warnings"),
                                                                    ("info", "Info"), ("debug", "Debug")]))
        vt = b.toggle("vk_validation")
        has_vl = have_validation_layer()
        vt.setEnabled(has_vl or win.cfg["vk_validation"])
        dx.add_row("Vulkan validation layer", "Very slow; reports Vulkan API misuse in the log."
                   + ("" if has_vl else " Not installed: sudo dnf install vulkan-validation-layers"), vt)
        v.addWidget(dx)

        # Movies
        mv = Card("Movies", "Skipping a movie makes the game think the file is missing, so it moves on.")
        self.mv_note = QLabel("")
        self.mv_note.setObjectName("note")
        self.mv_note.setWordWrap(True)
        mv.add(self.mv_note)
        mv.add_row("Skip the long movies", f"{INTRO_MOVIE} (after the studio logos) and {OPENING_MOVIE} "
                   "(the ~1 minute opening of a new game).", b.toggle("skip_intro_movie"))
        mv.add_row("Skip the studio logos", f"{LOGO_MOVIE}: the Turn 10 / Microsoft Studios logos.",
                   b.toggle("skip_logo_movie"))
        mv.add_row("Other movies", "Names (or parts of names) of other .bk2 movies, comma separated; "
                   "'all' skips every movie.", b.line("skip_extra_movies", "e.g. Festival_Montage"), wide=True)
        v.addWidget(mv)

        # Logging
        lg = Card("Runtime logging", "FL_LOG channels. Everything the game prints goes to the session log; "
                                     "busy channels slow the game down.")
        lgrid_box = QWidget()
        lgrid = QGridLayout(lgrid_box)
        lgrid.setContentsMargins(0, 4, 0, 4)
        lgrid.setHorizontalSpacing(18)
        lgrid.setVerticalSpacing(6)
        for i, (key, tip) in enumerate(LOG_CHANNELS):
            lgrid.addWidget(b.member("log_channels", key, key, tip), i // 5, i % 5)
        lg.add(lgrid_box)
        lg.add_row("Other channels", "Comma separated (e.g. cbwrite,alias,firstwrite).",
                   b.line("log_custom", "channel,channel"), wide=True)
        logs_btn = button("Open logs folder", "folder-open")
        logs_btn.clicked.connect(lambda: open_path(LOG_DIR))
        lg.add_row("Session logs", f"{LOG_DIR} (the newest {KEEP_LOGS} are kept)", logs_btn)
        v.addWidget(lg)

        # Automation
        au = Card("Testing and automation")
        au.add_row("Exit after frame", "FL_EXIT_AFTER_FRAME: quit after this many frames.",
                   b.spin("exit_after_frame", 0, 10_000_000, "", "Off", 100))
        au.add_row("Screenshots at frames", "FL_SCREENSHOT_AT_FRAME: frame numbers, comma separated; "
                   "PNGs go to screenshots/ in the checkout.", b.line("screenshot_frames", "14500,16000"), wide=True)
        ap_box = QWidget()
        aph = QHBoxLayout(ap_box)
        aph.setContentsMargins(0, 0, 0, 0)
        aph.setSpacing(8)
        ap = b.line("autopress", "seconds:Button[/hold], e.g. 40:A,42:A")
        preset = button("Get past menus", tip=AUTOPRESS_MENUS)
        preset.clicked.connect(lambda: ap.setText(AUTOPRESS_MENUS))
        aph.addWidget(ap, 1)
        aph.addWidget(preset)
        au.add_row("Auto-press buttons", "FL_AUTOPRESS: timed controller presses. Buttons: A B X Y LB RB "
                   "Menu View, RT/LT triggers, SteerL/SteerR.", ap_box, wide=True)
        au.add_row("Separate test save", "Run with its own save folder (XDG_DATA_HOME) so tests never touch "
                   "your real save.", b.toggle("test_profile"))
        box, _ = path_field(b, "test_profile_dir", True, "Folder for the test save")
        au.add_row("Test save folder", "", box, wide=True)
        v.addWidget(au)

        # Environment
        en = Card("Environment", "Extra environment variables and runtime arguments.")
        self.table = QTableWidget(0, 3)
        self.table.setHorizontalHeaderLabels(["On", "Variable", "Value"])
        self.table.verticalHeader().hide()
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        self.table.horizontalHeader().setSectionResizeMode(2, QHeaderView.ResizeMode.Stretch)
        self.table.setMinimumHeight(150)
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self._fill_env()
        self.table.itemChanged.connect(self._env_changed)
        en.add(self.table)
        eh = QHBoxLayout()
        add = button("Add variable", "list-add")
        rem = button("Remove selected", "list-remove")
        add.clicked.connect(self._env_add)
        rem.clicked.connect(self._env_remove)
        eh.addWidget(add)
        eh.addWidget(rem)
        eh.addStretch(1)
        ew = QWidget()
        ew.setLayout(eh)
        en.add(ew)
        en.add_row("Extra arguments", "Appended to the runtime's command line.",
                   b.line("extra_args", "e.g. --exe other.exe"), wide=True)
        v.addWidget(en)

        # Command preview
        cm = Card("Command", "Exactly what Play runs with the settings above.")
        self.preview = QPlainTextEdit()
        self.preview.setReadOnly(True)
        self.preview.setObjectName("console")
        self.preview.setFont(QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont))
        self.preview.setMinimumHeight(140)
        cm.add(self.preview)
        self.notes = QLabel("")
        self.notes.setObjectName("note")
        self.notes.setWordWrap(True)
        cm.add(self.notes)
        ch = QHBoxLayout()
        copy = button("Copy as one line", "edit-copy")
        copy.clicked.connect(lambda: QGuiApplication.clipboard().setText(self.plan.shell(False)))
        ch.addWidget(copy)
        ch.addStretch(1)
        cw = QWidget()
        cw.setLayout(ch)
        cm.add(cw)
        v.addWidget(cm)

        # Build
        bd = Card("Build", "Rebuild the runtime from this checkout (ninja). The output shows under Logs.")
        self.build_btn = button("Rebuild runtime", "run-build")
        self.build_btn.clicked.connect(win.rebuild)
        self.build_row = bd.add_row("Rebuild", "", self.build_btn)
        v.addWidget(bd)
        v.addStretch(1)

        b.changed.connect(self.update_state)
        self.update_state()

    def _fill_env(self) -> None:
        self.table.blockSignals(True)
        self.table.setRowCount(0)
        for row in self.win.cfg["env"]:
            self._env_row(row.get("on", True), row.get("name", ""), row.get("value", ""))
        self.table.blockSignals(False)

    def _env_row(self, on: bool, name: str, value: str) -> None:
        r = self.table.rowCount()
        self.table.insertRow(r)
        chk = QTableWidgetItem()
        chk.setFlags(Qt.ItemFlag.ItemIsUserCheckable | Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable)
        chk.setCheckState(Qt.CheckState.Checked if on else Qt.CheckState.Unchecked)
        self.table.setItem(r, 0, chk)
        self.table.setItem(r, 1, QTableWidgetItem(name))
        self.table.setItem(r, 2, QTableWidgetItem(value))

    def _env_changed(self, *_) -> None:
        rows = []
        for r in range(self.table.rowCount()):
            items = [self.table.item(r, c) for c in range(3)]
            if None in items:
                continue
            rows.append({"on": items[0].checkState() == Qt.CheckState.Checked, "name": items[1].text(),
                         "value": items[2].text()})
        self.win.binder.set("env", rows)

    def _env_add(self) -> None:
        self.table.blockSignals(True)
        self._env_row(True, "", "")
        self.table.blockSignals(False)
        self.table.editItem(self.table.item(self.table.rowCount() - 1, 1))

    def _env_remove(self) -> None:
        rows = sorted({i.row() for i in self.table.selectedIndexes()}, reverse=True)
        for r in rows:
            self.table.removeRow(r)
        self._env_changed()

    def update_state(self, key: str = "") -> None:
        cfg = self.win.cfg
        custom = cfg["mh_layout"] == "custom"
        for cb in self.mh_checks:
            cb.setEnabled(custom)
        rt = self.win.rt
        if rt.dev_build:
            self.mv_note.setText("This runtime is a developer build, so movie skipping works. Developer builds "
                                 "are for this machine only: don't share the binary.")
        elif rt.ok:
            self.mv_note.setText("This runtime is a release build: movie skipping only works in builds made with "
                                 "cmake -DFL_DEV_SKIP_MOVIES=ON.")
        else:
            self.mv_note.setText("")
        self.plan = build_launch(cfg, rt)
        if self.preview.toPlainText() != self.plan.shell():
            self.preview.setPlainText(self.plan.shell())
        msgs = [f"⚠ {p}" for p in self.plan.problems] + [f"ℹ {n}" for n in self.plan.notes]
        self.notes.setText("\n".join(msgs))
        self.notes.setVisible(bool(msgs))
        if rt.build_dir:
            self.build_row.set_desc(f"ninja -C {rt.build_dir}")
            self.build_btn.setEnabled(not self.win.building)
        else:
            self.build_row.set_desc("The runtime isn't in a ninja build folder.")
            self.build_btn.setEnabled(False)


class SavesPage(QWidget):
    def __init__(self, win: "MainWindow"):
        super().__init__()
        self.win = win
        v = QVBoxLayout(self)
        v.setContentsMargins(34, 28, 34, 28)
        v.setSpacing(16)
        v.addWidget(page_header("Saves", "Back up, restore or start over. Nothing here deletes a save: "
                                         "replaced saves are moved to the backups first."))

        cur = Card("Current save")
        self.cur_path = QLabel("")
        self.cur_path.setObjectName("muted")
        self.cur_path.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.cur_info = QLabel("")
        self.cur_info.setObjectName("infoValue")
        cur.add(self.cur_info)
        cur.add(self.cur_path)
        ch = QHBoxLayout()
        ch.setContentsMargins(0, 10, 0, 4)
        self.b_backup = button("Back up now", "document-save")
        self.b_fresh = button("Start fresh…", "view-refresh", tip="Moves the current save to the backups; the "
                                                                 "game then starts a new one")
        b_open = button("Open folder", "folder-open")
        self.b_backup.clicked.connect(self.backup)
        self.b_fresh.clicked.connect(self.fresh)
        b_open.clicked.connect(lambda: open_path(save_root(self.win.cfg)))
        for w in (self.b_backup, self.b_fresh, b_open):
            ch.addWidget(w)
        ch.addStretch(1)
        cw = QWidget()
        cw.setLayout(ch)
        cur.add(cw)
        v.addWidget(cur)

        bk = Card("Backups")
        self.tree = QTreeWidget()
        self.tree.setHeaderLabels(["Backup", "Saved", "Size"])
        self.tree.setRootIsDecorated(False)
        self.tree.setAlternatingRowColors(False)
        self.tree.header().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.tree.header().setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        self.tree.header().setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
        self.tree.setMinimumHeight(220)
        self.tree.itemSelectionChanged.connect(self.update_buttons)
        bk.add(self.tree)
        bh = QHBoxLayout()
        bh.setContentsMargins(0, 10, 0, 4)
        self.b_restore = button("Restore", "edit-undo")
        self.b_open_bk = button("Open", "folder-open")
        self.b_trash = button("Move to trash", "user-trash")
        b_refresh = button("Refresh", "view-refresh")
        self.b_restore.clicked.connect(self.restore)
        self.b_open_bk.clicked.connect(lambda: self.selected() and open_path(self.selected()))
        self.b_trash.clicked.connect(self.trash)
        b_refresh.clicked.connect(self.refresh)
        for w in (self.b_restore, self.b_open_bk, self.b_trash):
            bh.addWidget(w)
        bh.addStretch(1)
        bh.addWidget(b_refresh)
        bw = QWidget()
        bw.setLayout(bh)
        bk.add(bw)
        v.addWidget(bk, 1)
        self.refresh()

    def backups(self) -> list[Path]:
        cfg = self.win.cfg
        found = []
        root = backups_root(cfg)
        if root.is_dir():
            found += [p for p in root.iterdir() if p.is_dir()]
        # Copies made by hand next to the save folder (forza-linux.<something>).
        found += [p for p in data_home(cfg).glob("forza-linux.*") if p.is_dir()]
        return sorted(found, key=lambda p: p.stat().st_mtime, reverse=True)

    def selected(self) -> Path | None:
        items = self.tree.selectedItems()
        return Path(items[0].data(0, Qt.ItemDataRole.UserRole)) if items else None

    def refresh(self) -> None:
        cfg = self.win.cfg
        root = save_root(cfg)
        self.cur_path.setText(str(root) + ("   (separate test save)" if cfg["test_profile"] else ""))
        if root.is_dir():
            profiles = [p for p in (root / "saves").glob("*") if p.is_dir()] if (root / "saves").is_dir() else []
            self.cur_info.setText(f"{len(profiles)} profile{'s' if len(profiles) != 1 else ''} · "
                                  f"{human_size(dir_size(root))} · last saved {human_ago(dir_mtime(root))}")
        else:
            self.cur_info.setText("No save yet: the game makes one the first time you play")
        self.tree.clear()
        for p in self.backups():
            it = QTreeWidgetItem([p.name, time.strftime("%d %b %Y %H:%M", time.localtime(p.stat().st_mtime)),
                                  human_size(dir_size(p))])
            it.setData(0, Qt.ItemDataRole.UserRole, str(p))
            self.tree.addTopLevelItem(it)
        self.update_buttons()

    def update_buttons(self) -> None:
        running = self.win.proc is not None
        has = self.selected() is not None
        exists = save_root(self.win.cfg).is_dir()
        self.b_backup.setEnabled(exists)
        self.b_fresh.setEnabled(exists and not running)
        self.b_restore.setEnabled(has and not running)
        self.b_open_bk.setEnabled(has)
        self.b_trash.setEnabled(has)

    def _stamp(self, label: str) -> Path:
        name = time.strftime("%Y-%m-%d %H-%M-%S") + (f" {label}" if label else "")
        name = re.sub(r"[/\\:]", "-", name)
        return backups_root(self.win.cfg) / name

    def _copy_current(self, label: str) -> Path:
        dest = self._stamp(label)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(save_root(self.win.cfg), dest, symlinks=True)
        return dest

    def _move_current(self, label: str) -> Path:
        dest = self._stamp(label)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(save_root(self.win.cfg)), dest)
        return dest

    def backup(self) -> None:
        label, ok = QInputDialog.getText(self, "Back up save", "Name (optional):")
        if not ok:
            return
        if self.win.proc and QMessageBox.question(
                self, "Game is running", "The game is running and may be writing its save right now. "
                "Back up anyway?") != QMessageBox.StandardButton.Yes:
            return
        try:
            dest = self._copy_current(label.strip())
        except OSError as e:
            QMessageBox.critical(self, "Backup failed", str(e))
            return
        self.win.console.note(f"save backed up to {dest}")
        self.refresh()

    def fresh(self) -> None:
        if QMessageBox.question(self, "Start fresh", "Move the current save to the backups? The game starts a "
                                "new save next time (you can restore the old one from here).") \
                != QMessageBox.StandardButton.Yes:
            return
        try:
            dest = self._move_current("before fresh start")
        except OSError as e:
            QMessageBox.critical(self, "Failed", str(e))
            return
        self.win.console.note(f"current save moved to {dest}")
        self.refresh()

    def restore(self) -> None:
        src = self.selected()
        if not src:
            return
        root = save_root(self.win.cfg)
        if QMessageBox.question(self, "Restore save", f"Replace the current save with “{src.name}”?\n\nThe "
                                "current save is moved to the backups first.") != QMessageBox.StandardButton.Yes:
            return
        try:
            if root.exists():
                self._move_current("before restore")
            shutil.copytree(src, root, symlinks=True)
        except OSError as e:
            QMessageBox.critical(self, "Restore failed", str(e))
            return
        self.win.console.note(f"restored save from {src}")
        self.refresh()

    def trash(self) -> None:
        src = self.selected()
        if not src or QMessageBox.question(self, "Move to trash", f"Move the backup “{src.name}” to the "
                                           "trash?") != QMessageBox.StandardButton.Yes:
            return
        from PySide6.QtCore import QFile
        if not QFile.moveToTrash(str(src)):
            QMessageBox.critical(self, "Failed", "Couldn't move it to the trash.")
        self.refresh()


class LogsPage(QWidget):
    def __init__(self, win: "MainWindow"):
        super().__init__()
        self.win = win
        v = QVBoxLayout(self)
        v.setContentsMargins(34, 28, 34, 28)
        v.setSpacing(12)
        v.addWidget(page_header("Logs", "Live output from the game and from builds. Every session is saved "
                                        "in full to its own log file."))
        bar = QHBoxLayout()
        bar.setSpacing(10)
        self.search = QLineEdit()
        self.search.setPlaceholderText("Filter lines…")
        self.search.setClearButtonEnabled(True)
        self.search.setMinimumWidth(260)
        self.errors = QCheckBox("Errors only")
        self.follow = QCheckBox("Follow")
        self.follow.setChecked(True)
        clear = button("Clear", "edit-clear-all")
        open_log = button("Open session log", "document-open")
        open_dir = button("Logs folder", "folder-open")
        bar.addWidget(self.search, 1)
        bar.addWidget(self.errors)
        bar.addWidget(self.follow)
        bar.addWidget(clear)
        bar.addWidget(open_log)
        bar.addWidget(open_dir)
        v.addLayout(bar)
        self.console = Console()
        v.addWidget(self.console, 1)
        self.search.textChanged.connect(self._refilter)
        self.errors.toggled.connect(self._refilter)
        self.follow.toggled.connect(lambda on: setattr(self.console, "follow", on))
        clear.clicked.connect(self.console.wipe)
        open_log.clicked.connect(self._open_log)
        open_dir.clicked.connect(lambda: open_path(LOG_DIR))

    def _refilter(self, *_) -> None:
        self.console.filter = self.search.text().lower()
        self.console.errors_only = self.errors.isChecked()
        self.console.refilter()

    def _open_log(self) -> None:
        last = self.win.session_log or (self.win.cfg.get("last_session") or {}).get("log")
        if last and Path(last).is_file():
            open_path(Path(last))


def open_path(p: Path) -> None:
    if p.exists():
        QDesktopServices.openUrl(QUrl.fromLocalFile(str(p)))


# ---------------------------------------------------------------- main window

NAV = [("Play", "media-playback-start"), ("Settings", "configure"), ("Developer", "code-context"),
       ("Saves", "document-save"), ("Logs", "utilities-terminal")]


class MainWindow(QMainWindow):
    def __init__(self, cfg: dict):
        super().__init__()
        self.cfg = cfg
        self.binder = Binder(cfg)
        self.rt = RuntimeInfo.probe(cfg["runtime"])
        self.proc: QProcess | None = None
        self.build_proc: QProcess | None = None
        self.building = False
        self.session: dict | None = None
        self.session_log: str | None = None
        self.log_file = None
        self.recent_errors: collections.deque[str] = collections.deque(maxlen=6)
        self.stop_timer: QTimer | None = None

        self.setWindowTitle("forza-linux launcher")  # Qt appends " — Forza Horizon 2"
        self.setWindowIcon(QIcon(str(icon_path(cfg))))
        self.resize(1280, 860)
        self.setMinimumSize(1080, 720)

        root = QWidget()
        root.setObjectName("root")
        h = QHBoxLayout(root)
        h.setContentsMargins(0, 0, 0, 0)
        h.setSpacing(0)
        h.addWidget(self._sidebar())
        self.stack = QStackedWidget()
        h.addWidget(self.stack, 1)
        self.setCentralWidget(root)

        self.logs = LogsPage(self)
        self.console = self.logs.console
        self.play_page = PlayPage(self)
        self.settings_page = SettingsPage(self)
        self.dev_page = DevPage(self)
        self.saves_page = SavesPage(self)
        for w in (self.play_page, self.settings_page, self.dev_page, self.saves_page, self.logs):
            self.stack.addWidget(w)

        self.binder.changed.connect(self._on_setting)
        self.ticker = QTimer(self)
        self.ticker.setInterval(1000)
        self.ticker.timeout.connect(self._tick)
        self.ticker.start()
        self._load_previous_log()
        self.refresh()
        self.go(int(cfg.get("page", 0)) if 0 <= int(cfg.get("page", 0)) < len(NAV) else 0)

    # ---- layout

    def _sidebar(self) -> QWidget:
        side = QFrame()
        side.setObjectName("sidebar")
        side.setFixedWidth(232)
        v = QVBoxLayout(side)
        v.setContentsMargins(16, 22, 16, 18)
        v.setSpacing(4)
        brand = QHBoxLayout()
        brand.setSpacing(12)
        logo = QLabel()
        pm = QIcon(str(icon_path(self.cfg))).pixmap(46, 46)
        logo.setPixmap(pm)
        brand.addWidget(logo)
        bt = QVBoxLayout()
        bt.setSpacing(0)
        n = QLabel("Forza Horizon 2")
        n.setObjectName("brand")
        s = QLabel("forza-linux launcher")
        s.setObjectName("muted")
        bt.addWidget(n)
        bt.addWidget(s)
        brand.addLayout(bt, 1)
        v.addLayout(brand)
        v.addSpacing(22)
        self.nav = QButtonGroup(self)
        self.nav.setExclusive(True)
        for i, (name, icon) in enumerate(NAV):
            b = QPushButton(f"  {name}")
            b.setObjectName("nav")
            b.setIcon(QIcon.fromTheme(icon))
            b.setIconSize(QSize(18, 18))
            b.setCheckable(True)
            b.setCursor(Qt.CursorShape.PointingHandCursor)
            self.nav.addButton(b, i)
            v.addWidget(b)
        self.nav.idClicked.connect(self.go)
        v.addStretch(1)
        self.side_status = QLabel("")
        self.side_status.setObjectName("sideStatus")
        self.side_status.setWordWrap(True)
        v.addWidget(self.side_status)
        return side

    def go(self, i: int) -> None:
        self.stack.setCurrentIndex(i)
        self.nav.button(i).setChecked(True)
        self.binder.set("page", i)
        if i == 3:
            self.saves_page.refresh()

    # ---- state

    def _on_setting(self, key: str) -> None:
        if key == "runtime":
            self.rt = RuntimeInfo.probe(self.cfg["runtime"])
        if key in ("game_dir", "runtime", "test_profile", "test_profile_dir"):
            self.refresh()
        else:
            self.refresh_play()

    def refresh(self) -> None:
        gi = game_info(self.cfg["game_dir"])
        self.play_page.hero.set_art(gi.splash)
        self.settings_page.game_row.set_desc(
            f"✓ {gi.name}{' · version ' + gi.version if gi.version else ''}" if gi.ok else f"⚠ {gi.reason}")
        if self.rt.ok:
            kind = "developer build" if self.rt.dev_build else "release build"
            built = time.strftime("%d %b %H:%M", time.localtime(self.rt.mtime))
            self.settings_page.rt_row.set_desc(f"✓ {kind} · built {built}{' · ' + self.rt.git if self.rt.git else ''}")
        else:
            self.settings_page.rt_row.set_desc(f"⚠ {self.rt.reason}")
        self.saves_page.refresh()
        self.dev_page.update_state()
        self.refresh_play()

    def refresh_play(self) -> None:
        p = self.play_page
        cfg = self.cfg
        gi = game_info(cfg["game_dir"])
        plan = build_launch(cfg, self.rt)
        p.title.setVisible(not p.hero.has_art())

        if self.proc:
            up = human_duration(time.time() - self.session["start"])
            p.pill.set(f"Running · {up}" if self.proc.state() == QProcess.ProcessState.Running else "Starting…",
                       "run")
            p.play.setText("STOPPING…" if self.stop_timer else "STOP")
            p.play.setEnabled(not self.stop_timer)
            p.play.setProperty("state", "running")
        else:
            p.play.setText("PLAY")
            p.play.setProperty("state", "")
            p.play.setEnabled(not plan.problems)
            if plan.problems:
                p.pill.set("Not ready: " + plan.problems[0], "warn")
            else:
                p.pill.set("Ready to race", "ok")
        p.play.style().unpolish(p.play)
        p.play.style().polish(p.play)

        bits = []
        if "FL_SKIP_MOVIES" in plan.env:
            bits.append("long movies skipped")
        if "mangohud" in plan.cmd:
            bits.append("MangoHud")
        if "gamemoderun" in plan.cmd:
            bits.append("GameMode")
        if plan.env.get("FL_FRAME_PACING") == "off":
            bits.append("uncapped (too fast)")
        elif plan.env.get("FL_FRAME_PACING") == "dynamic":
            bits.append("dynamic fps" + (f" ≤ {cfg['fps_cap']}" if cfg["fps_cap"] else ""))
        if plan.env.get("FL_DISPLAY_MODE") == "borderless":
            bits.append("borderless")
        elif plan.env.get("FL_DISPLAY_MODE") == "fullscreen":
            bits.append("fullscreen" + (" " + plan.env["FL_WINDOW"].replace("x", " × ") if "FL_WINDOW" in plan.env else ""))
        elif "FL_WINDOW" in plan.env:
            bits.append(plan.env["FL_WINDOW"].replace("x", " × "))
        if "FL_LOG" in plan.env:
            bits.append(f"logging {plan.env['FL_LOG']}")
        if cfg["test_profile"]:
            bits.append("test save")
        p.summary.setText("  ·  ".join(bits))

        skip_ok = self.rt.dev_build
        p.skip_chip.toggle.setEnabled(skip_ok)
        p.skip_chip.desc.setText("Intro and opening" if skip_ok else "Developer builds only")
        p.skip_chip.setToolTip("" if skip_ok else "Movie skipping only works in runtimes built with "
                                                  "cmake -DFL_DEV_SKIP_MOVIES=ON")

        p.c_game.set(gi.name if gi.ok else "Not set up", (f"Version {gi.version}" if gi.version else "")
                     if gi.ok else gi.reason, "" if gi.ok else "warn")
        if self.rt.ok:
            p.c_runtime.set("Developer build" if self.rt.dev_build else "Release build",
                            f"Built {human_ago(self.rt.mtime)}" + (f" · {self.rt.git}" if self.rt.git else ""))
        else:
            p.c_runtime.set("Missing", self.rt.reason, "warn")
        root = save_root(cfg)
        if root.is_dir():
            p.c_save.set(human_size(dir_size(root)), f"Last saved {human_ago(dir_mtime(root))}"
                         + (" · test save" if cfg["test_profile"] else ""))
        else:
            p.c_save.set("No save yet", "Made the first time you play")
        last = cfg.get("last_session")
        if self.proc:
            p.c_last.set("Playing now", f"Started {time.strftime('%H:%M', time.localtime(self.session['start']))}",
                         "run")
        elif last:
            kind = {"ok": "", "crash": "bad", "error": "warn"}.get(last.get("kind", ""), "")
            p.c_last.set(last.get("result", "–"), f"{human_ago(last.get('end', 0))} · played "
                         f"{human_duration(last.get('duration', 0))}", kind)
        else:
            p.c_last.set("None yet", "")

        others = running_instances({self.proc.processId()} if self.proc else set())
        notes = []
        if others:
            notes.append(f"Another forza-linux is already running (pid {', '.join(map(str, others))}). Two copies "
                         "share the GPU, so both run slower.")
        # The game shows a new frame every 1/29.97 s: on a display whose refresh
        # isn't a multiple of that, frames stay up for uneven times (judder).
        screen = self.screen() or QGuiApplication.primaryScreen()
        hz = screen.refreshRate() if screen else 0
        if hz > 0 and not cfg["frame_pacing"]:
            per = hz / 29.97
            if abs(per - round(per)) > 0.05:
                notes.append(f"Your display runs at {hz:.0f} Hz, which 30 fps doesn't divide evenly, so motion "
                             "judders slightly. For perfectly smooth frames use a 60/120/240 Hz mode, or turn on "
                             "Adaptive Sync and play fullscreen.")
        p.banner.setText("\n".join(notes))
        p.banner.setVisible(bool(notes))

        if self.proc:
            self.side_status.setText("● Game running")
            self.side_status.setProperty("kind", "run")
        elif self.building:
            self.side_status.setText("● Building runtime…")
            self.side_status.setProperty("kind", "warn")
        else:
            self.side_status.setText(f"forza-linux {self.rt.git or ''}".strip())
            self.side_status.setProperty("kind", "")
        self.side_status.style().unpolish(self.side_status)
        self.side_status.style().polish(self.side_status)
        self.saves_page.update_buttons()

    def _tick(self) -> None:
        if self.proc or self.play_page.isVisible():
            self.refresh_play()

    # ---- running the game

    def play_or_stop(self) -> None:
        if self.proc:
            self.stop()
        else:
            self.play()

    def play(self) -> None:
        if not self.building and SELF.stat().st_mtime != SELF_MTIME:
            # The launcher was updated while open: restart into the new code, then play.
            save_config(self.cfg)
            os.execv(sys.executable, [sys.executable, str(SELF), "--play"])
        plan = build_launch(self.cfg, self.rt)
        if plan.problems:
            QMessageBox.warning(self, "Can't start yet", "\n".join(plan.problems))
            return
        others = running_instances()
        if others and self.cfg["warn_other_instance"]:
            r = QMessageBox.question(self, "Already running", f"forza-linux is already running (pid "
                                     f"{', '.join(map(str, others))}). Two copies share the GPU and both run "
                                     "slower. Start another one anyway?")
            if r != QMessageBox.StandardButton.Yes:
                return
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        self._prune_logs()
        self.session_log = str(LOG_DIR / time.strftime("session-%Y-%m-%d_%H-%M-%S.log"))
        self.log_file = open(self.session_log, "wb", buffering=0)  # live: tail -f works
        header = f"# forza-linux session {time.strftime('%Y-%m-%d %H:%M:%S')}\n# {plan.shell(False)}\n"
        self.log_file.write(header.encode())
        self.recent_errors.clear()

        proc = QProcess(self)
        env = QProcessEnvironment.systemEnvironment()
        for k, v in plan.env.items():
            env.insert(k, v)
        proc.setProcessEnvironment(env)
        proc.setWorkingDirectory(str(plan.cwd))
        proc.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        proc.readyReadStandardOutput.connect(self._on_output)
        proc.finished.connect(self._on_finished)
        proc.errorOccurred.connect(self._on_error)
        self.proc = proc
        self.session = {"start": time.time(), "log": self.session_log}
        self.console.note("─" * 60)
        self.console.note("starting: " + plan.shell(False))
        for n in plan.notes:
            self.console.note(n)
        proc.start(plan.cmd[0], plan.cmd[1:])
        self.refresh_play()
        if self.cfg["minimize_on_play"]:
            self.showMinimized()

    def stop(self) -> None:
        if not self.proc or self.stop_timer:
            return
        self.console.note("stopping the game…")
        self.proc.terminate()
        self.stop_timer = QTimer(self)
        self.stop_timer.setSingleShot(True)
        self.stop_timer.timeout.connect(lambda: self.proc and self.proc.kill())
        self.stop_timer.start(6000)
        self.refresh_play()

    def _on_output(self) -> None:
        if not self.proc:
            return
        data = bytes(self.proc.readAllStandardOutput())
        if self.log_file:
            self.log_file.write(data)
        text = data.decode("utf-8", errors="replace")
        self.console.feed(text)
        if "ERROR" in text or "FATAL" in text or "UNHANDLED" in text:
            for line in text.splitlines():
                if re.search(r"\b(ERROR|FATAL|UNHANDLED)\b", line):
                    self.recent_errors.append(line)

    def _on_error(self, err) -> None:
        if err == QProcess.ProcessError.FailedToStart and self.proc:
            self.console.note(f"failed to start: {self.proc.errorString()}")
            QMessageBox.critical(self, "Couldn't start the game", self.proc.errorString())
            self._finish({"result": "Failed to start", "kind": "error"})

    def _on_finished(self, code: int, status) -> None:
        if not self.proc:
            return
        self._on_output()
        if status == QProcess.ExitStatus.CrashExit and not self.stop_timer:
            res = {"result": "Crashed", "kind": "crash"}
        elif self.stop_timer:
            res = {"result": "Stopped", "kind": "ok"}
        elif code == 0:
            res = {"result": "Exited normally", "kind": "ok"}
        elif code > 128:
            res = {"result": f"Crashed (signal {code - 128})", "kind": "crash"}
        else:
            res = {"result": f"Exited with code {code}", "kind": "error"}
        self._finish(res)

    def _finish(self, res: dict) -> None:
        dur = time.time() - self.session["start"] if self.session else 0
        self.console.note(f"{res['result'].lower()} after {human_duration(dur)} · log: {self.session_log}")
        if res["kind"] == "crash" and self.recent_errors:
            self.console.note("last errors before the crash:")
            for line in self.recent_errors:
                self.console.feed(line + "\n")
        if self.log_file:
            self.log_file.close()
            self.log_file = None
        last = dict(res, end=time.time(), duration=dur, log=self.session_log)
        self.binder.set("last_session", last)
        if self.stop_timer:
            self.stop_timer.stop()
            self.stop_timer = None
        proc, self.proc = self.proc, None
        if proc:
            proc.deleteLater()
        if self.isMinimized():
            self.showNormal()
        if res["kind"] == "crash" and self.cfg["logs_on_crash"]:
            self.go(4)
        self.refresh()

    def _prune_logs(self) -> None:
        logs = sorted(LOG_DIR.glob("session-*.log"))
        for old in logs[:-(KEEP_LOGS - 1)]:
            try:
                old.unlink()
            except OSError:
                pass

    def _load_previous_log(self) -> None:
        last = self.cfg.get("last_session") or {}
        p = Path(last.get("log") or "")
        if not last.get("log") or not p.is_file():
            return
        try:
            with open(p, "rb") as f:
                f.seek(max(0, p.stat().st_size - 400_000))
                tail = f.read().decode("utf-8", errors="replace").split("\n")[-3000:]
        except OSError:
            return
        self.console.note(f"previous session ({last.get('result', '')}), last lines of {p}:")
        self.console.feed("\n".join(tail) + "\n")

    # ---- building

    def rebuild(self) -> None:
        if self.building or not self.rt.build_dir:
            return
        if not shutil.which("ninja"):
            QMessageBox.warning(self, "ninja not found", "Install ninja-build to rebuild from here.")
            return
        self.building = True
        bp = QProcess(self)
        bp.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        bp.readyReadStandardOutput.connect(
            lambda: self.console.feed("".join(f"[build] {l}\n" for l in
                                              bytes(bp.readAllStandardOutput()).decode(errors="replace")
                                              .splitlines())))
        bp.finished.connect(lambda code, _status: self._build_done(code))
        bp.errorOccurred.connect(lambda e: e == QProcess.ProcessError.FailedToStart and self._build_done(-1))
        self.build_proc = bp
        self.console.note(f"building: ninja -C {self.rt.build_dir}")
        bp.start("ninja", ["-C", str(self.rt.build_dir)])
        self.go(4)
        self.refresh_play()

    def _build_done(self, code: int) -> None:
        if not self.building:
            return
        self.building = False
        self.console.note("build finished" if code == 0 else f"build FAILED (exit {code})")
        if self.build_proc:
            self.build_proc.deleteLater()
            self.build_proc = None
        self.rt = RuntimeInfo.probe(self.cfg["runtime"])
        self.refresh()

    # ---- misc

    def install_shortcuts(self) -> None:
        try:
            done = install_shortcuts(self.cfg)
        except OSError as e:
            QMessageBox.critical(self, "Shortcuts", str(e))
            return
        self.setWindowIcon(QIcon(str(icon_path(self.cfg))))
        QMessageBox.information(self, "Shortcuts", "Done:\n\n" + "\n".join(done))

    def closeEvent(self, e) -> None:
        if self.proc:
            r = QMessageBox.question(self, "Game is running", "Forza Horizon 2 is still running. Closing the "
                                     "launcher stops the game. Stop it and quit?")
            if r != QMessageBox.StandardButton.Yes:
                e.ignore()
                return
            self.proc.terminate()
            if not self.proc.waitForFinished(6000):
                self.proc.kill()
                self.proc.waitForFinished(2000)
        save_config(self.cfg)
        e.accept()


# ---------------------------------------------------------------- theme

def write_theme_assets() -> Path:
    d = CACHE_DIR / "launcher-theme"
    d.mkdir(parents=True, exist_ok=True)
    svgs = {
        "down.svg": f'<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12"><path d="M2 4l4 4 4-4" '
                    f'fill="none" stroke="{C["muted"]}" stroke-width="1.8" stroke-linecap="round"/></svg>',
        "up.svg": f'<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12"><path d="M2 8l4-4 4 4" '
                  f'fill="none" stroke="{C["muted"]}" stroke-width="1.8" stroke-linecap="round"/></svg>',
        "check.svg": '<svg xmlns="http://www.w3.org/2000/svg" width="14" height="14"><path d="M3 7.5l2.6 2.6L11 '
                     '4.5" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" '
                     'stroke-linejoin="round"/></svg>',
    }
    for name, svg in svgs.items():
        (d / name).write_text(svg)
    return d


def stylesheet(assets: Path) -> str:
    a = assets.as_posix()
    grad = f"qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 {C['pink']}, stop:1 {C['orange']})"
    return f"""
    QWidget {{ color: {C['text']}; font-size: 10pt; }}
    #root, QStackedWidget, #pageInner, QScrollArea, QScrollArea > QWidget > QWidget {{ background: {C['bg']}; }}
    #sidebar {{ background: {C['side']}; border-right: 1px solid {C['line']}; }}
    QLabel#brand {{ font-size: 12pt; font-weight: 800; }}
    QLabel#muted {{ color: {C['muted']}; }}
    QLabel#h1 {{ font-size: 21pt; font-weight: 800; }}
    QLabel#sideStatus {{ color: {C['dim']}; font-size: 9pt; padding: 4px 8px; }}
    QLabel#sideStatus[kind="run"] {{ color: {C['green']}; }}
    QLabel#sideStatus[kind="warn"] {{ color: {C['amber']}; }}
    QPushButton#nav {{ text-align: left; padding: 10px 12px; border: none; border-radius: 10px;
        color: {C['muted']}; background: transparent; font-weight: 600; font-size: 10.5pt; }}
    QPushButton#nav:hover {{ background: {C['card']}; color: {C['text']}; }}
    QPushButton#nav:checked {{ color: #ffffff; background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
        stop:0 rgba(255,45,120,0.30), stop:1 rgba(255,138,0,0.06)); border-left: 3px solid {C['pink']}; }}
    #card {{ background: {C['card']}; border: 1px solid {C['line']}; border-radius: 16px; }}
    #chip, #info {{ background: {C['card']}; border: 1px solid {C['line']}; border-radius: 14px; }}
    #info:hover {{ border-color: #343c52; background: {C['card2']}; }}
    QFrame#sep {{ background: {C['line']}; border: none; }}
    QLabel#cardTitle {{ font-size: 13pt; font-weight: 800; }}
    QLabel#rowTitle {{ font-weight: 600; }}
    QLabel#infoTitle {{ color: {C['dim']}; font-size: 8.5pt; font-weight: 800; }}
    QLabel#infoValue {{ font-size: 13pt; font-weight: 700; }}
    QLabel#infoValue[kind="warn"] {{ color: {C['amber']}; }}
    QLabel#infoValue[kind="bad"] {{ color: {C['red']}; }}
    QLabel#infoValue[kind="run"] {{ color: {C['green']}; }}
    QLabel#note {{ color: {C['amber']}; background: rgba(255,176,32,0.08); border: 1px solid rgba(255,176,32,0.25);
        border-radius: 10px; padding: 8px 12px; }}
    QLabel#banner {{ color: {C['amber']}; background: rgba(255,176,32,0.10); border: 1px solid rgba(255,176,32,0.35);
        border-radius: 12px; padding: 10px 14px; font-weight: 600; }}
    QLabel#heroTitle {{ font-size: 34pt; font-weight: 900; font-style: italic; color: #ffffff; }}
    QLabel#heroTag {{ font-size: 13pt; font-weight: 600; color: #f3f4f8; }}
    QLabel#heroNote {{ color: #d5d8e2; font-weight: 600; }}
    QLabel#pill {{ background: rgba(12,14,19,0.72); border: 1px solid rgba(255,255,255,0.14); border-radius: 13px;
        padding: 5px 14px; font-weight: 700; color: {C['green']}; }}
    QLabel#pill[kind="warn"] {{ color: {C['amber']}; }}
    QLabel#pill[kind="run"] {{ color: {C['pink']}; }}
    QPushButton {{ background: {C['card2']}; border: 1px solid #2b3246; border-radius: 10px; padding: 8px 16px;
        font-weight: 600; }}
    QPushButton:hover {{ background: #232a3d; border-color: #3a4360; }}
    QPushButton:pressed {{ background: #1a1f2d; }}
    QPushButton:disabled {{ color: {C['dim']}; background: #151925; border-color: {C['line']}; }}
    QPushButton#play {{ font-size: 19pt; font-weight: 900; padding: 14px 54px; border: none;
        border-radius: 16px; color: #ffffff; background: {grad}; min-width: 150px; }}
    QPushButton#play:hover {{ background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #ff4f8f, stop:1 #ffa233); }}
    QPushButton#play:pressed {{ background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #e0206a, stop:1 #e67a00); }}
    QPushButton#play[state="running"] {{ background: #2a2f40; border: 2px solid {C['red']}; color: {C['red']}; }}
    QPushButton#play:disabled {{ background: #2a2f40; color: {C['dim']}; }}
    QLineEdit, QSpinBox, QComboBox, QPlainTextEdit, QTableWidget, QTreeWidget {{ background: #0f1219;
        border: 1px solid #262d3e; border-radius: 9px; padding: 6px 9px; selection-background-color: {C['pink']};
        selection-color: #ffffff; }}
    QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus {{ border-color: {C['pink']}; }}
    QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled {{ color: {C['dim']}; }}
    QPlainTextEdit#console {{ background: #090b10; border-radius: 12px; padding: 8px; }}
    QComboBox::drop-down {{ border: none; width: 26px; }}
    QComboBox::down-arrow {{ image: url({a}/down.svg); width: 12px; height: 12px; }}
    QComboBox QAbstractItemView {{ background: {C['card']}; border: 1px solid {C['line']}; padding: 4px;
        selection-background-color: rgba(255,45,120,0.35); outline: none; }}
    QSpinBox::up-button, QSpinBox::down-button {{ subcontrol-origin: border; width: 20px; border: none;
        background: transparent; }}
    QSpinBox::up-button {{ subcontrol-position: top right; }}
    QSpinBox::down-button {{ subcontrol-position: bottom right; }}
    QSpinBox::up-arrow {{ image: url({a}/up.svg); width: 10px; height: 10px; }}
    QSpinBox::down-arrow {{ image: url({a}/down.svg); width: 10px; height: 10px; }}
    QCheckBox {{ spacing: 9px; }}
    QCheckBox::indicator {{ width: 18px; height: 18px; border-radius: 5px; border: 1px solid #343c52;
        background: #0f1219; }}
    QCheckBox::indicator:hover {{ border-color: {C['pink']}; }}
    QCheckBox::indicator:checked {{ background: {grad}; border: none; image: url({a}/check.svg); }}
    QCheckBox:disabled {{ color: {C['dim']}; }}
    QCheckBox::indicator:disabled {{ background: #151925; border-color: {C['line']}; }}
    QCheckBox::indicator:checked:disabled {{ background: #3a3040; }}
    QSlider::groove:horizontal {{ height: 6px; background: #232838; border-radius: 3px; }}
    QSlider::sub-page:horizontal {{ background: {grad}; border-radius: 3px; }}
    QSlider::handle:horizontal {{ background: #ffffff; width: 16px; height: 16px; margin: -5px 0; border-radius: 8px; }}
    QHeaderView::section {{ background: {C['card']}; color: {C['muted']}; border: none;
        border-bottom: 1px solid {C['line']}; padding: 7px 9px; font-weight: 700; }}
    QTreeWidget::item, QTableWidget::item {{ padding: 6px 4px; }}
    QTreeWidget::item:selected, QTableWidget::item:selected {{ background: rgba(255,45,120,0.28); color: #ffffff; }}
    QTableWidget {{ gridline-color: {C['line']}; }}
    QScrollBar:vertical {{ background: transparent; width: 11px; margin: 4px 2px; }}
    QScrollBar::handle:vertical {{ background: #2b3246; border-radius: 3px; min-height: 36px; }}
    QScrollBar::handle:vertical:hover {{ background: #3a4360; }}
    QScrollBar:horizontal {{ background: transparent; height: 11px; margin: 2px 4px; }}
    QScrollBar::handle:horizontal {{ background: #2b3246; border-radius: 3px; min-width: 36px; }}
    QScrollBar::add-line, QScrollBar::sub-line {{ width: 0; height: 0; }}
    QScrollBar::add-page, QScrollBar::sub-page {{ background: transparent; }}
    QToolTip {{ background: {C['card2']}; color: {C['text']}; border: 1px solid #343c52; padding: 6px 8px;
        border-radius: 6px; }}
    QMessageBox, QInputDialog {{ background: {C['card']}; }}
    """


def apply_theme(app: QApplication) -> None:
    app.setStyle("Fusion")
    pal = QPalette()
    roles = {
        QPalette.ColorRole.Window: C["bg"], QPalette.ColorRole.WindowText: C["text"],
        QPalette.ColorRole.Base: "#0f1219", QPalette.ColorRole.AlternateBase: C["card"],
        QPalette.ColorRole.Text: C["text"], QPalette.ColorRole.Button: C["card2"],
        QPalette.ColorRole.ButtonText: C["text"], QPalette.ColorRole.Highlight: C["pink"],
        QPalette.ColorRole.HighlightedText: "#ffffff", QPalette.ColorRole.ToolTipBase: C["card2"],
        QPalette.ColorRole.ToolTipText: C["text"], QPalette.ColorRole.PlaceholderText: C["dim"],
        QPalette.ColorRole.Link: C["pink"],
    }
    for role, col in roles.items():
        pal.setColor(role, QColor(col))
    for role in (QPalette.ColorRole.Text, QPalette.ColorRole.WindowText, QPalette.ColorRole.ButtonText):
        pal.setColor(QPalette.ColorGroup.Disabled, role, QColor(C["dim"]))
    app.setPalette(pal)
    app.setStyleSheet(stylesheet(write_theme_assets()))


# ---------------------------------------------------------------- entry point

def main() -> int:
    ap = argparse.ArgumentParser(description="forza-linux launcher")
    ap.add_argument("--install-shortcuts", action="store_true", help="add the app menu entry and desktop icon")
    ap.add_argument("--print-command", action="store_true", help="print what Play would run, then exit")
    ap.add_argument("--play", action="store_true", help="start the game as soon as the launcher opens")
    ap.add_argument("--grab", metavar="DIR", help=argparse.SUPPRESS)  # dev: save each page as a PNG
    args = ap.parse_args()

    QGuiApplication.setDesktopFileName(APP_ID)
    app = QApplication(sys.argv[:1])
    app.setApplicationName("forza-linux-launcher")
    app.setApplicationDisplayName("Forza Horizon 2")
    cfg = load_config()

    if args.print_command:
        plan = build_launch(cfg, RuntimeInfo.probe(cfg["runtime"]))
        print(plan.shell())
        for p in plan.problems:
            print("problem:", p, file=sys.stderr)
        for n in plan.notes:
            print("note:", n, file=sys.stderr)
        return 1 if plan.problems else 0
    if args.install_shortcuts:
        for line in install_shortcuts(cfg):
            print(line)
        return 0

    apply_theme(app)
    win = MainWindow(cfg)
    win.show()
    if args.play:
        QTimer.singleShot(300, win.play)
    if args.grab:
        out = Path(args.grab)
        out.mkdir(parents=True, exist_ok=True)

        def shoot():
            for i, (name, _) in enumerate(NAV):
                win.go(i)
                for _ in range(5):
                    app.processEvents()
                win.grab().save(str(out / f"{i}-{name.lower()}.png"))
            app.quit()
        QTimer.singleShot(600, shoot)
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
