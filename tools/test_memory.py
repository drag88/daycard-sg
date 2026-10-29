"""python3 -m unittest discover -s tools -p test_memory.py"""
import io
import json
from pathlib import Path
import socket
import tempfile
import threading
import unittest
from unittest.mock import patch

from PIL import Image

import memory


class MemoryTests(unittest.TestCase):
    def test_metadata_rejects_bad_dates_and_unrenderable_captions(self):
        self.assertEqual(json.loads(memory.metadata("2024-02-29", "  Leap day  ")),
                         {"date": "2024-02-29", "caption": "Leap day"})
        for date in ["2023-02-29", "2024-13", "2026-04-31", "2024-9", "2024-09junk", "1899-12", "２０２４-09"]:
            with self.subTest(date=date), self.assertRaises(ValueError):
                memory.metadata(date, "A trip")
        for caption in ["", " ", "x" * 57, "a\nb", "Caf\u00e9", "a\0b"]:
            with self.subTest(caption=caption), self.assertRaises(ValueError):
                memory.metadata("2024-09", caption)

    def test_photo_corrects_orientation_and_exports_baseline_without_exif(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "portrait.jpg"
            image = Image.new("RGB", (1800, 900), "green")
            exif = image.getexif()
            exif[274] = 6
            exif[270] = "Private description"
            image.save(source, "JPEG", progressive=True, exif=exif)
            data = memory.prepare_photo(source)
            with Image.open(io.BytesIO(data)) as ready:
                self.assertEqual(ready.size, (600, 1200))
                self.assertFalse(ready.info.get("progressive"))
                self.assertFalse(ready.getexif())

    def test_card_copy_is_repeatable_and_preserves_conflicting_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            name = "memory-test.jpg"
            meta = memory.metadata("2024-09", "Beach trip")
            memory.save_to_card(tmp, name, b"photo", meta)
            memory.save_to_card(tmp, name, b"photo", meta)
            photo = Path(tmp) / "memories" / name
            self.assertEqual(json.loads(photo.with_suffix(".jpg.json").read_bytes())["caption"], "Beach trip")
            photo.write_bytes(b"existing")
            with self.assertRaises(ValueError):
                memory.save_to_card(tmp, name, b"photo", meta)
            self.assertEqual(photo.read_bytes(), b"existing")
            self.assertFalse(list(Path(tmp).rglob("*.part")))

    def run_board(self, device, action):
        client, peer = socket.socketpair()
        errors = []

        def serve():
            try:
                with peer, peer.makefile("rwb", buffering=0) as stream:
                    device(stream)
            except BaseException as exc:
                errors.append(exc)

        thread = threading.Thread(target=serve)
        thread.start()
        try:
            with client:
                action(memory.Board(client.fileno()))
        finally:
            thread.join(3)
        self.assertFalse(thread.is_alive(), "fake device did not finish")
        if errors:
            raise errors[0]

    def test_usb_handles_split_lines_log_noise_and_combined_final_ack(self):
        data = b"sample jpeg" * 99

        def device(stream):
            self.assertEqual(stream.read(1), b"G")
            stream.write(b"battery 4.10 V\r\ngarden: v1 total 0; memory-v1\r\n")
            self.assertEqual(stream.readline(), b"Umemory-test.jpg\n")
            self.assertEqual(int(stream.readline()), len(data))
            stream.write(b"memory: ready\r\n")
            got = b""
            while len(got) < len(data):
                want = min(memory.CHUNK, len(data) - len(got))
                chunk = b""
                while len(chunk) < want:
                    chunk += stream.read(want - len(chunk))
                got += chunk
                status = b"battery 4.11 V\r\nmemory: ack\r\n"
                if len(got) == len(data):
                    status += b"memory: saved\r\n"
                stream.write(status)
            self.assertEqual(got, data)

        def action(board):
            # Force every read to fragment protocol lines, including ack + final saved in one buffer.
            read = memory.os.read
            with patch.object(memory.os, "read", side_effect=lambda fd, n: read(fd, min(n, 7))):
                board.check()
                board.upload("memory-test.jpg", data)

        self.run_board(device, action)

    def test_usb_probes_before_sending_personal_content(self):
        def device(stream):
            self.assertEqual(stream.read(1), b"G")
            stream.write(b"garden: old firmware\r\n")
            self.assertEqual(stream.read(1), b"")

        def action(board):
            with self.assertRaises(RuntimeError):
                board.check()

        self.run_board(device, action)

    def test_usb_stops_on_disk_failure_and_can_skip_existing_files(self):
        def device(stream):
            self.assertEqual(stream.readline(), b"Umemory-test.jpg\n")
            self.assertEqual(stream.readline(), b"300\n")
            stream.write(b"memory: ready\r\n")
            self.assertEqual(len(stream.read(256)), 256)
            stream.write(b"memory: failed\r\n")
            self.assertEqual(stream.read(1), b"")

        def action(board):
            with self.assertRaises(RuntimeError):
                board.upload("memory-test.jpg", b"x" * 300)

        self.run_board(device, action)

        def existing(stream):
            stream.readline(), stream.readline()
            stream.write(b"memory: exists\r\n")
            self.assertEqual(stream.read(1), b"")

        self.run_board(existing, lambda board: board.upload("memory-test.jpg", b"x"))


if __name__ == "__main__":
    unittest.main()
