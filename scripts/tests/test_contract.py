import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class PlasmaContractTests(unittest.TestCase):
    def test_connector_only_launches_paper(self):
        source = (ROOT / "src/skwdvideoitem.cpp").read_text()
        qml = (ROOT / "wallpaper/contents/ui/main.qml").read_text()
        self.assertIn("present-plasma", source)
        self.assertIn("m_process.start(m_paper, arguments)", source)
        self.assertIn("JSON.stringify(root.currentAssignment.assignment)", qml)
        for forbidden in (
            "skwd-wall-vk",
            "skwd-wall-still",
            "skwd-paper-tinier",
            "--video-stream",
            "--frame-stream",
            "--scene",
        ):
            self.assertNotIn(forbidden, source)
            self.assertNotIn(forbidden, qml)

    def test_cpu_stream_keeps_pixels_until_upload(self):
        source = (ROOT / "src/skwdvideoitem.cpp").read_text()
        self.assertIn("const QImage image = borrowed.copy();", source)
        self.assertIn(
            "frame.size() != qsizetype(frameWidth) * qsizetype(frameHeight) * 4",
            source,
        )
        self.assertIn(
            "frameHeight, frameWidth * 4, QImage::Format_RGBA8888", source
        )
        self.assertIn("m_frame.clear();", source)


if __name__ == "__main__":
    unittest.main()
