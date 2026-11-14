import unittest
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
AIA_PATH = PROJECT_ROOT / "mit_app" / "SPOILALERT_Updated.aia"
BLOCKS_PATH = "src/appinventor/ai_user/SPOILALERT/Screen1.bky"
NS = "{http://www.w3.org/1999/xhtml}"


class AppInventorContractTests(unittest.TestCase):
    def test_app_reads_prediction_field_and_discloses_dataset_estimate_status(self):
        with zipfile.ZipFile(AIA_PATH) as aia:
            blocks = ET.fromstring(aia.read(BLOCKS_PATH))

        fields = [node for node in blocks.iter(f"{NS}field") if node.get("name") == "TEXT"]
        text_values = [node.text or "" for node in fields]
        self.assertIn("prediction", text_values)
        self.assertTrue(any("Dataset-based estimate" in value for value in text_values))
        self.assertTrue(any("not a food-safety determination" in value.lower() for value in text_values))


if __name__ == "__main__":
    unittest.main()
