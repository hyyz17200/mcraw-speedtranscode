#!/usr/bin/env python3
"""Import a release candidate into DaVinci Resolve and report clip metadata."""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path


def load_resolve_module():
    program_data = Path(os.environ.get("PROGRAMDATA", r"C:\ProgramData"))
    api_root = program_data / "Blackmagic Design/DaVinci Resolve/Support/Developer/Scripting"
    module_root = api_root / "Modules"
    library = Path(r"C:\Program Files\Blackmagic Design\DaVinci Resolve\fusionscript.dll")
    if not module_root.is_dir() or not library.is_file():
        raise RuntimeError("DaVinci Resolve scripting API is not installed")
    sys.path.insert(0, str(module_root))
    os.environ.setdefault("RESOLVE_SCRIPT_API", str(api_root))
    os.environ.setdefault("RESOLVE_SCRIPT_LIB", str(library))
    import DaVinciResolveScript as resolve_script  # type: ignore

    return resolve_script


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("movie", type=Path)
    parser.add_argument("--project", default="mcraw-release-gate")
    args = parser.parse_args()
    movie = args.movie.resolve()
    if not movie.is_file():
        parser.error(f"movie does not exist: {movie}")

    resolve_script = load_resolve_module()
    resolve = resolve_script.scriptapp("Resolve")
    if resolve is None:
        raise RuntimeError("DaVinci Resolve is not running or external scripting is disabled")

    manager = resolve.GetProjectManager()
    if manager is None:
        raise RuntimeError("DaVinci Resolve project manager is unavailable")

    manager.CloseProject(manager.GetCurrentProject())
    manager.DeleteProject(args.project)
    project = manager.CreateProject(args.project)
    if project is None:
        raise RuntimeError(f"could not create temporary project: {args.project}")

    result: dict[str, object]
    try:
        media_pool = project.GetMediaPool()
        clips = media_pool.ImportMedia([str(movie)])
        if not clips or len(clips) != 1:
            raise RuntimeError("Resolve did not import exactly one media-pool item")
        clip = clips[0]
        properties = dict(clip.GetClipProperty())
        timeline = media_pool.CreateTimelineFromClips("compatibility", [clip])
        if timeline is None:
            raise RuntimeError("Resolve imported the clip but could not create a timeline")
        project.SaveProject()
        result = {
            "ok": True,
            "resolve_product": resolve.GetProductName(),
            "resolve_version": resolve.GetVersionString(),
            "movie": str(movie),
            "clip_properties": properties,
            "timeline_start_frame": timeline.GetStartFrame(),
            "timeline_end_frame": timeline.GetEndFrame(),
        }
        print(json.dumps(result, indent=2, sort_keys=True))
    finally:
        manager.CloseProject(project)
        manager.DeleteProject(args.project)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
