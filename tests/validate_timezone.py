#!/usr/bin/env python3

"""Validate the bounded timezone catalog and its 2026 civil-time behavior."""

import calendar
import datetime
import os
import pathlib
import re
import time


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main" / "time_manager.cpp").read_text(encoding="utf-8")
TIMEZONES = re.findall(
    r'\{"([^"]+)", "([^"]+)", "([A-Z]{3,4})"\},',
    SOURCE,
)
IANA_REFERENCE = {
    "UTC": "Etc/UTC",
    "US/Hawaii": "Pacific/Honolulu",
    "US/Alaska": "America/Anchorage",
    "US/Pacific": "America/Los_Angeles",
    "US/Mountain": "America/Denver",
    "US/Arizona": "America/Phoenix",
    "US/Central": "America/Chicago",
    "Mexico/Central": "America/Mexico_City",
    "US/Eastern": "America/New_York",
    "Canada/Atlantic": "America/Halifax",
    "Colombia": "America/Bogota",
    "Argentina": "America/Argentina/Buenos_Aires",
    "Brazil/East": "America/Sao_Paulo",
    "UK": "Europe/London",
    "Europe/Central": "Europe/Paris",
    "Europe/Eastern": "Europe/Helsinki",
    "SouthAfrica": "Africa/Johannesburg",
    "EastAfrica": "Africa/Nairobi",
    "Russia/Moscow": "Europe/Moscow",
    "Gulf": "Asia/Dubai",
    "Pakistan": "Asia/Karachi",
    "India": "Asia/Kolkata",
    "Bangladesh": "Asia/Dhaka",
    "Indochina": "Asia/Bangkok",
    "China": "Asia/Shanghai",
    "Japan": "Asia/Tokyo",
    "Australia/West": "Australia/Perth",
    "Australia/Central": "Australia/Adelaide",
    "Australia/East": "Australia/Sydney",
    "NewZealand": "Pacific/Auckland",
}


def set_timezone(value: str) -> None:
    os.environ["TZ"] = value
    time.tzset()


def local_fields(epoch: int) -> tuple[int, ...]:
    value = time.localtime(epoch)
    return (
        value.tm_year,
        value.tm_mon,
        value.tm_mday,
        value.tm_hour,
        value.tm_min,
        value.tm_sec,
        value.tm_isdst,
    )


if not 20 <= len(TIMEZONES) <= 30:
    raise SystemExit(f"expected 20-30 timezones, found {len(TIMEZONES)}")

names = [entry[0] for entry in TIMEZONES]
if len(names) != len(set(names)):
    raise SystemExit("timezone human names must be unique")
if set(names) != set(IANA_REFERENCE):
    missing = sorted(set(IANA_REFERENCE) - set(names))
    extra = sorted(set(names) - set(IANA_REFERENCE))
    raise SystemExit(f"timezone catalog mismatch: missing={missing} extra={extra}")

previous_timezone = os.environ.get("TZ")
try:
    start = datetime.datetime(2026, 1, 1, 12)
    sample_epochs = [
        calendar.timegm((start + datetime.timedelta(days=day)).timetuple())
        for day in range(0, 366, 7)
    ]

    for human_name, posix_rule, short_name in TIMEZONES:
        if any(character.isspace() for character in human_name):
            raise SystemExit(f"{human_name!r} is not a single-token name")
        if not posix_rule.startswith(short_name):
            raise SystemExit(
                f"{human_name}: short name {short_name} does not match {posix_rule}"
            )

        set_timezone(posix_rule)
        posix_values = [local_fields(epoch) for epoch in sample_epochs]
        january_zone = time.localtime(sample_epochs[2]).tm_zone
        july_zone = time.localtime(sample_epochs[28]).tm_zone
        if short_name not in {january_zone, july_zone}:
            raise SystemExit(
                f"{human_name}: {short_name} is not the standard abbreviation "
                f"({january_zone}, {july_zone})"
            )

        set_timezone(IANA_REFERENCE[human_name])
        iana_values = [local_fields(epoch) for epoch in sample_epochs]
        if posix_values != iana_values:
            differing = next(
                index
                for index, pair in enumerate(zip(posix_values, iana_values))
                if pair[0] != pair[1]
            )
            timestamp = datetime.datetime.fromtimestamp(
                sample_epochs[differing], datetime.timezone.utc
            )
            raise SystemExit(
                f"{human_name}: POSIX rule differs from "
                f"{IANA_REFERENCE[human_name]} near {timestamp.isoformat()}"
            )
finally:
    if previous_timezone is None:
        os.environ.pop("TZ", None)
    else:
        os.environ["TZ"] = previous_timezone
    time.tzset()

print(f"timezone catalog and 2026 transitions: ok ({len(TIMEZONES)} rules)")
