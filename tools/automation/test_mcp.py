"""Self-check for mcp_server.py without the game: a fake folder answers command files.

  uv run -q --with mcp --with numpy --with pillow --with scipy python tools/automation/test_mcp.py

It covers what can break without a dev build: the file handshake, the player-write refusal, path safety and
the errors a model is meant to act on. The game itself is tested by a case and a replay.
"""
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import mcp_server as server


class FakeGame:
    """A folder that answers every .cmd with one `ok:` line, as the game's automation does."""

    def __init__(self, root):
        self.root = Path(root)
        self.link = self.root / "PhantomFX-automation"
        self.link.mkdir(parents=True)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stop.is_set():
            for file in sorted(self.link.glob("*.cmd")):
                try:
                    lines = file.read_text(encoding="utf-8").splitlines()
                    file.unlink()
                except (FileNotFoundError, PermissionError):  # Still being renamed, or already gone
                    continue
                file.with_suffix(".result").write_text("\n".join(f"ok: {line}" for line in lines) + "\n", encoding="utf-8")
            time.sleep(0.01)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)


def payload(result):
    return result.structured_content


def with_player(root):
    server.GAMES["player"] = Path(root)
    server.fx.game_folder = Path(root)


def test_run_and_refusals():
    with tempfile.TemporaryDirectory() as temp:
        game = FakeGame(temp)
        with_player(temp)
        try:
            result = server.run(lines=["status"], target="player", timeout_s=5)
            assert not result.is_error, result.content[0].text
            assert "ok: status" in payload(result)["result"], payload(result)

            result = server.run(lines="setting a=1\nstatus", target="player", timeout_s=5)
            assert result.is_error and payload(result)["code"] == "NEEDS_CONFIRM", payload(result)

            result = server.run(lines=["setting a=1"], target="player", timeout_s=5, confirm=True)
            assert not result.is_error, payload(result)

            result = server.session(action="start", target="player")
            assert result.is_error and payload(result)["code"] == "WRONG_TARGET", payload(result)

            result = server.capture(kind="image", name="../escape", target="player")
            assert result.is_error and payload(result)["code"] == "BAD_PATH", payload(result)
        finally:
            game.close()


def test_setting_reads_the_ini():
    with tempfile.TemporaryDirectory() as temp:
        (Path(temp) / "PhantomFX.ini").write_text("ray_tracing=1\nlight_radius=0.5\n", encoding="utf-8")
        with_player(temp)
        result = server.setting(action="get", key="ray_tracing", target="player")
        assert not result.is_error, payload(result)
        assert payload(result)["value"] == "1", payload(result)

        result = server.setting(action="get", key="missing", target="player")
        assert result.is_error and payload(result)["code"] == "NO_SETTING", payload(result)


def test_status_says_no_game():
    with tempfile.TemporaryDirectory() as temp:
        with_player(temp)
        original = server.fx.run

        def no_game(lines, timeout):
            raise TimeoutError("no dev build")

        server.fx.run = no_game
        try:
            result = server.session(action="status", target="player")
            assert not result.is_error, payload(result)
            assert payload(result)["responds"] is False, payload(result)
        finally:
            server.fx.run = original


def test_job_list_is_empty():
    result = server.job(action="list")
    assert not result.is_error and payload(result)["jobs"] == [], payload(result)


if __name__ == "__main__":
    for check in (test_run_and_refusals, test_setting_reads_the_ini, test_status_says_no_game, test_job_list_is_empty):
        check()
        print(f"{check.__name__}: ok")

    print("mcp_server: all checks passed")
