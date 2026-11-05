"""The bench's protocol codec, and its agreement with the C core's codec on the same frames."""

import pytest
from hypothesis import given
from hypothesis import strategies as st

from bench import core
from bench.frames import (
    FLAGS_ALL,
    LINE_MAX,
    SENSOR_OFF,
    BlFrame,
    BsFrame,
    FrameError,
    LineSplitter,
    OxFrame,
    checksum,
    parse_bl,
    parse_bs,
    parse_ox,
)

ox_frames = st.builds(
    OxFrame,
    clock=st.integers(0, 0xFFFFFFFF),
    spo2=st.none() | st.integers(0, 100),
    pr=st.none() | st.integers(0, 400),
    pi_centi=st.none() | st.integers(0, 2000),
    flags=st.integers(0, FLAGS_ALL),
)


def test_checksums_match_the_protocol_examples() -> None:
    assert checksum("OX,7261,93,156,1.48,00") == 0x12
    assert checksum("BL,412,305,300,0") == 0x0C
    assert checksum("BS,412,301,N") == 0x76


def test_protocol_examples_parse() -> None:
    assert parse_ox("$OX,7261,93,156,1.48,00*12") == OxFrame(7261, 93, 156, 148, 0)
    assert parse_bl("$BL,412,305,300,0*0C") == BlFrame(412, 305, 300, 0)
    assert parse_bs("$BS,412,301,N*76") == BsFrame(412, 301, "N")


@given(ox_frames)
def test_ox_frames_round_trip(f: OxFrame) -> None:
    data = f.encode()
    assert len(data) <= LINE_MAX and data.endswith(b"\r\n")
    assert parse_ox(data.decode().rstrip("\r\n")) == f


@given(ox_frames)
def test_the_c_core_parses_every_frame_the_bench_writes(f: OxFrame) -> None:
    assert core.c_ox_parse(f.encode().decode().rstrip("\r\n")) == (f.clock, f.spo2, f.pr, f.pi_centi, f.flags)


@given(ox_frames)
def test_the_bench_and_the_c_core_write_identical_bytes(f: OxFrame) -> None:
    assert core.c_ox_format(f.clock, f.spo2, f.pr, f.pi_centi, f.flags) == f.encode()


@given(st.text(alphabet="$OX,0123456789.-*ABCDEF\r", max_size=40))
def test_both_parsers_accept_and_reject_the_same_lines(line: str) -> None:
    try:
        ours: tuple[int, int | None, int | None, int | None, int] | None = None
        f = parse_ox(line)
        ours = (f.clock, f.spo2, f.pr, f.pi_centi, f.flags)
    except FrameError:
        ours = None
    assert core.c_ox_parse(line) == ours


@pytest.mark.parametrize(
    ("line", "reason"),
    [
        ("OX,7261,93,156,1.48,00*12", "no_start"),
        ("$OX,7261,93,156,1.48,00", "no_checksum"),
        ("$OX,7261,93,156,1.48,00*13", "checksum"),
        ("$*00", "empty"),
        ("$" + "1" * 80 + "*00", "too_long"),
    ],
)
def test_malformed_lines_are_rejected_with_their_reason(line: str, reason: str) -> None:
    with pytest.raises(FrameError, match=reason):
        parse_ox(line)


def _signed(body: str) -> str:
    return f"${body}*{checksum(body):02X}"


@pytest.mark.parametrize(
    "body",
    [
        "BL,7261,93,156,1.48,00",
        "OX,7261,93,156,1.48",
        "OX,7261,101,156,1.48,00",
        "OX,7261,93,401,1.48,00",
        "OX,7261,93,156,1.4,00",
        "OX,7261,93,156,20.01,00",
        "OX,7261,93,156,1.48,20",
        "OX,7261,93,156,1.48,0",
        "OX,7261,9a,156,1.48,00",
    ],
)
def test_bad_fields_are_rejected_even_with_a_good_checksum(body: str) -> None:
    with pytest.raises(FrameError):
        parse_ox(_signed(body))
    assert core.c_ox_parse(_signed(body)) is None


@pytest.mark.parametrize(
    "frame",
    [
        OxFrame(1, 101, 150, 100),
        OxFrame(1, 95, 401, 100),
        OxFrame(1, 95, 150, 2001),
        OxFrame(1, 95, 150, 100, 0x20),
        OxFrame(-1, 95, 150, 100),
    ],
)
def test_out_of_range_frames_are_not_written(frame: OxFrame) -> None:
    with pytest.raises(ValueError):
        frame.encode()


def test_absent_values_are_written_as_dashes() -> None:
    assert b",--,--,--,01*" in OxFrame(5, None, None, None, SENSOR_OFF).encode()


@pytest.mark.parametrize(
    "frame",
    [BlFrame(65536, 300, 300, 0), BlFrame(1, 209, 300, 0), BlFrame(1, 300, 300, 4), BsFrame(1, 300, "X")],
)
def test_out_of_range_blender_frames_are_not_written(frame: BlFrame | BsFrame) -> None:
    with pytest.raises(ValueError):
        frame.encode()


@given(st.integers(0, 65535), st.integers(210, 1000), st.integers(210, 1000), st.integers(0, 3))
def test_blender_commands_round_trip(seq: int, fio2: int, backup: int, alarm: int) -> None:
    f = BlFrame(seq, fio2, backup, alarm)
    assert parse_bl(f.encode().decode().rstrip("\r\n")) == f


@given(st.integers(0, 65535), st.integers(0, 1000), st.sampled_from("NHF"))
def test_blender_status_round_trips(seq: int, fio2: int, state: str) -> None:
    f = BsFrame(seq, fio2, state)
    assert parse_bs(f.encode().decode().rstrip("\r\n")) == f


@pytest.mark.parametrize(
    ("parse", "body"),
    [
        (parse_bl, "BS,1,300,300,0"),
        (parse_bl, "BL,1,300,300"),
        (parse_bl, "BL,1,209,300,0"),
        (parse_bs, "BL,1,300,N"),
        (parse_bs, "BS,1,300"),
        (parse_bs, "BS,1,300,X"),
    ],
)
def test_blender_frames_with_bad_fields_are_rejected(parse: object, body: str) -> None:
    with pytest.raises(FrameError):
        parse(_signed(body))  # type: ignore[operator]


def test_splitter_drops_noise_restarts_on_dollar_and_drops_long_lines() -> None:
    s = LineSplitter()
    data = b"noise$OX,1*00\r\n$BL,partial$BS,2*00\n$" + b"A" * 100 + b"\r\n$OK*00\r\n"
    assert s.feed(data) == ["$OX,1*00", "$BS,2*00", "$OK*00"]


def test_splitter_keeps_a_line_split_across_reads() -> None:
    s = LineSplitter()
    assert s.feed(b"$OX,7261,93,1") == []
    assert s.feed(b"56,1.48,00*12\r\n") == ["$OX,7261,93,156,1.48,00*12"]
