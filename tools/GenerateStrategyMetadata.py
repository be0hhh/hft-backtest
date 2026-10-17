#!/usr/bin/env python3
"""Generate bounded Backtest strategy metadata from trading-core profiles."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from dataclasses import dataclass, field


MAX_PROFILES = 64
MAX_PARAMS = 16
MAX_INDICATORS = 8
MAX_PARAM_GROUPS = 8
MAX_FILE_BYTES = 64 * 1024
MAX_TEXT_BYTES = 1024
IDENTIFIER = re.compile(r"^[a-z0-9_]+$")


class ProfileError(RuntimeError):
    pass


@dataclass
class ParamPolicy:
    exclusive_group: int = 0
    default_active: bool = True


@dataclass
class Profile:
    file_name: str
    strategy_type: str = ""
    display_name: str = ""
    min_sessions: int = 0
    max_sessions: int = 0
    recorder_legs: int = 0
    params: list[tuple[str, str]] = field(default_factory=list)
    indicators: list[tuple[str, str]] = field(default_factory=list)
    param_groups: list[tuple[int, str]] = field(default_factory=list)
    param_policies: dict[str, ParamPolicy] = field(default_factory=dict)


def fail(path: pathlib.Path, line: int, message: str) -> ProfileError:
    location = f"{path}:{line}" if line else str(path)
    return ProfileError(f"{location}: {message}")


def bounded_text(path: pathlib.Path, line: int, name: str, value: str) -> str:
    if len(value.encode("utf-8")) > MAX_TEXT_BYTES:
        raise fail(path, line, f"{name} exceeds {MAX_TEXT_BYTES} UTF-8 bytes")
    return value


def parse_uint8(path: pathlib.Path, line: int, name: str, value: str) -> int:
    if not value.isascii() or not value.isdigit():
        raise fail(path, line, f"{name} must be an unsigned decimal integer")
    parsed = int(value, 10)
    if parsed > 255:
        raise fail(path, line, f"{name} exceeds 255")
    return parsed


def parse_bool(path: pathlib.Path, line: int, name: str, value: str) -> bool:
    lowered = value.lower()
    if lowered in {"1", "true", "yes", "on"}:
        return True
    if lowered in {"0", "false", "no", "off"}:
        return False
    raise fail(path, line, f"{name} must be an explicit boolean")


def parse_indicator(path: pathlib.Path, line: int, value: str) -> tuple[str, str]:
    separator = value.find(":")
    if separator < 0:
        separator = value.find(",")
    if separator < 0:
        identifier = value.strip()
        label = identifier
    else:
        identifier = value[:separator].strip()
        label = value[separator + 1 :].strip() or identifier
    if not IDENTIFIER.fullmatch(identifier):
        raise fail(path, line, "indicator id must match [a-z0-9_]+")
    return (
        bounded_text(path, line, "indicator id", identifier),
        bounded_text(path, line, "indicator label", label),
    )


def parse_group(path: pathlib.Path, line: int, value: str) -> tuple[int, str]:
    identifier, separator, key = value.partition(":")
    if not separator:
        raise fail(path, line, "param_group must be <1..255>:<key>")
    group_id = parse_uint8(path, line, "param_group id", identifier.strip())
    key = key.strip()
    if group_id == 0 or not IDENTIFIER.fullmatch(key):
        raise fail(path, line, "param_group must use a nonzero id and canonical key")
    return group_id, key


def parse_profile(path: pathlib.Path, root: pathlib.Path) -> Profile:
    raw = path.read_bytes()
    if not raw or len(raw) > MAX_FILE_BYTES:
        raise fail(path, 0, f"profile must contain 1..{MAX_FILE_BYTES} bytes")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise fail(path, error.start + 1, "profile is not valid UTF-8") from error

    try:
        relative = path.relative_to(root).as_posix()
    except ValueError as error:
        raise fail(path, 0, "profile is outside the declared strategy root") from error
    # Consumers resolve this name against trading-core/StrategyProfiles.
    profile = Profile(file_name=relative)
    section = ""
    seen_params: set[str] = set()

    for line_number, original in enumerate(text.splitlines(), 1):
        stripped = original.strip()
        if not stripped:
            continue
        template_comment = stripped.startswith("#")
        if template_comment:
            clean = stripped[1:].strip()
        else:
            clean = original.split("#", 1)[0].strip()
        if not clean:
            continue
        if clean.startswith("[") or clean.endswith("]"):
            if not (clean.startswith("[") and clean.endswith("]")):
                raise fail(path, line_number, "malformed section header")
            section = clean[1:-1].strip().lower()
            if not section:
                raise fail(path, line_number, "empty section name")
            continue
        if "=" not in clean:
            if template_comment:
                continue
            raise fail(path, line_number, "active profile line must be key=value")
        key, value = (part.strip() for part in clean.split("=", 1))
        key = key.lower()
        bounded_text(path, line_number, "value", value)

        if section == "strategy":
            if not template_comment and key == "type":
                if profile.strategy_type:
                    raise fail(path, line_number, "duplicate strategy type")
                if not IDENTIFIER.fullmatch(value):
                    raise fail(path, line_number, "strategy type must match [a-z0-9_]+")
                profile.strategy_type = value
            elif key not in {"type", "enabled", "inputs"}:
                if not IDENTIFIER.fullmatch(key):
                    raise fail(path, line_number, "strategy parameter key must match [a-z0-9_]+")
                if key in seen_params:
                    raise fail(path, line_number, f"duplicate strategy parameter {key!r}")
                seen_params.add(key)
                profile.params.append((key, value))
        elif not template_comment and section == "metadata":
            if key in {"display_name_ru", "display_name"}:
                if profile.display_name:
                    raise fail(path, line_number, "duplicate display name")
                profile.display_name = bounded_text(path, line_number, "display name", value)
            elif key == "min_sessions":
                profile.min_sessions = parse_uint8(path, line_number, key, value)
            elif key == "max_sessions":
                profile.max_sessions = parse_uint8(path, line_number, key, value)
            elif key == "indicator":
                profile.indicators.append(parse_indicator(path, line_number, value))
            elif key == "param_group":
                profile.param_groups.append(parse_group(path, line_number, value))
        elif not template_comment and section.startswith("parameter."):
            param_key = section[len("parameter.") :]
            if not IDENTIFIER.fullmatch(param_key):
                raise fail(path, line_number, "parameter section key must match [a-z0-9_]+")
            policy = profile.param_policies.setdefault(param_key, ParamPolicy())
            if key == "exclusive_group":
                policy.exclusive_group = parse_uint8(path, line_number, key, value)
            elif key == "default_active":
                policy.default_active = parse_bool(path, line_number, key, value)
        elif not template_comment and section == "portfolio.recorder" and key == "legs":
            legs = [part.strip() for part in value.split(",") if part.strip()]
            profile.recorder_legs = max(profile.recorder_legs, len(legs))

    if not profile.strategy_type:
        raise fail(path, 0, "missing active [strategy] type")
    if len(profile.params) > MAX_PARAMS:
        raise fail(path, 0, f"strategy has more than {MAX_PARAMS} parameters")
    if len(profile.indicators) > MAX_INDICATORS:
        raise fail(path, 0, f"strategy has more than {MAX_INDICATORS} indicators")
    if len(profile.param_groups) > MAX_PARAM_GROUPS:
        raise fail(path, 0, f"strategy has more than {MAX_PARAM_GROUPS} parameter groups")
    unknown_policies = sorted(set(profile.param_policies) - seen_params)
    if unknown_policies:
        raise fail(path, 0, f"parameter policy without a template value: {unknown_policies[0]}")

    if profile.min_sessions == 0 and profile.max_sessions == 0:
        inferred = max(1, profile.recorder_legs)
        profile.min_sessions = inferred
        profile.max_sessions = inferred
    elif profile.min_sessions == 0:
        profile.min_sessions = profile.max_sessions
    elif profile.max_sessions == 0:
        profile.max_sessions = profile.min_sessions
    if profile.max_sessions < profile.min_sessions:
        raise fail(path, 0, "max_sessions is less than min_sessions")
    return profile


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def emit_array(items: list[str], capacity: int, type_name: str) -> str:
    body = ",\n".join(f"        {item}" for item in items)
    if body:
        body += ",\n"
    return f"std::array<{type_name}, {capacity}>{{{{\n{body}      }}}}"


def emit_profile(profile: Profile) -> str:
    params: list[str] = []
    for key, value in profile.params:
        policy = profile.param_policies.get(key, ParamPolicy())
        params.append(
            "GeneratedParamTemplate{" + ", ".join(
                [
                    cpp_string(key),
                    cpp_string(value),
                    str(policy.exclusive_group),
                    "true" if policy.default_active else "false",
                ]
            ) + "}"
        )
    indicators = [
        f"GeneratedIndicatorTemplate{{{cpp_string(identifier)}, {cpp_string(label)}}}"
        for identifier, label in profile.indicators
    ]
    groups = [
        f"GeneratedParamGroupTemplate{{{group_id}, {cpp_string(key)}}}"
        for group_id, key in profile.param_groups
    ]
    display = profile.display_name or profile.strategy_type
    return f"""GeneratedStrategyTemplate{{
      {cpp_string(profile.file_name)},
      {cpp_string(profile.strategy_type)},
      {cpp_string(display)},
      {profile.min_sessions},
      {profile.max_sessions},
      {len(params)},
      {emit_array(params, MAX_PARAMS, 'GeneratedParamTemplate')},
      {len(groups)},
      {emit_array(groups, MAX_PARAM_GROUPS, 'GeneratedParamGroupTemplate')},
      {len(indicators)},
      {emit_array(indicators, MAX_INDICATORS, 'GeneratedIndicatorTemplate')}
    }}"""


def generate(profiles: list[Profile]) -> str:
    rows = ",\n    ".join(emit_profile(profile) for profile in profiles)
    return f"""// Generated by apps/hft-backtest/tools/generate_strategy_metadata.py.
// Do not edit. The generator validates every canonical trading-core profile.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hft_backtest::generated {{

inline constexpr std::size_t kMaximumStrategies = {MAX_PROFILES}u;
inline constexpr std::size_t kMaximumParams = {MAX_PARAMS}u;
inline constexpr std::size_t kMaximumIndicators = {MAX_INDICATORS}u;
inline constexpr std::size_t kMaximumParamGroups = {MAX_PARAM_GROUPS}u;

struct GeneratedParamTemplate final {{
  const char* key;
  const char* defaultValue;
  std::uint8_t exclusiveGroup;
  bool defaultActive;
}};

struct GeneratedParamGroupTemplate final {{
  std::uint8_t id;
  const char* key;
}};

struct GeneratedIndicatorTemplate final {{
  const char* id;
  const char* label;
}};

struct GeneratedStrategyTemplate final {{
  const char* fileName;
  const char* strategyType;
  const char* displayName;
  std::uint8_t minSessionCount;
  std::uint8_t maxSessionCount;
  std::size_t paramCount;
  std::array<GeneratedParamTemplate, kMaximumParams> params;
  std::size_t paramGroupCount;
  std::array<GeneratedParamGroupTemplate, kMaximumParamGroups> paramGroups;
  std::size_t indicatorCount;
  std::array<GeneratedIndicatorTemplate, kMaximumIndicators> indicators;
}};

inline constexpr std::array<GeneratedStrategyTemplate, {len(profiles)}> kStrategyTemplates{{{{
    {rows}
}}}};

}}  // namespace hft_backtest::generated
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile-root", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--required-strategy", action="append", default=[])
    parser.add_argument("profiles", nargs="+", type=pathlib.Path)
    args = parser.parse_args()

    root = args.profile_root.resolve(strict=True)
    paths = sorted({path.resolve(strict=True) for path in args.profiles})
    if len(paths) > MAX_PROFILES:
        raise ProfileError(f"profile count exceeds {MAX_PROFILES}")
    profiles = [parse_profile(path, root) for path in paths]
    seen_types: set[str] = set()
    for profile in profiles:
        if profile.strategy_type in seen_types:
            raise ProfileError(f"duplicate strategy type {profile.strategy_type!r}")
        seen_types.add(profile.strategy_type)
    required_types = set(args.required_strategy)
    for required_type in required_types:
        if not IDENTIFIER.fullmatch(required_type):
            raise ProfileError(f"invalid required strategy name {required_type!r}")
    missing_types = sorted(required_types - seen_types)
    if missing_types:
        raise ProfileError(
            f"missing canonical profile for registered strategy {missing_types[0]!r}"
        )

    content = generate(profiles)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    temporary.write_text(content, encoding="utf-8", newline="\n")
    temporary.replace(args.output)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ProfileError) as error:
        print(f"strategy metadata generation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
