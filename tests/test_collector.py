import pytest
import os
import time
import glob
import shutil
from src.astralog_collector import TelemetryCollector, OUTPUT_DIR

@pytest.fixture(autouse=True)
def setup_and_cleanup():
    """
    Ensure the output directory is clean before and after each test 
    using the path defined in the collector source.
    """
    # Pre-test cleanup
    if os.path.exists(OUTPUT_DIR):
        shutil.rmtree(OUTPUT_DIR)
    
    yield
    
    # cleanup after test
    if os.path.exists(OUTPUT_DIR):
        shutil.rmtree(OUTPUT_DIR)

def test_count_based_flush():
    """Verify that files are created in the shared OUTPUT_DIR."""
    limit = 2
    collector = TelemetryCollector(mode="count", limit=limit)
    
    collector.handle_message("test_1")
    collector.handle_message("test_2") # Trigger flush
    
    found_files = glob.glob(os.path.join(OUTPUT_DIR, "raw_data_*.txt"))
    
    assert len(found_files) == 1
    assert os.path.exists(found_files[0])

def test_time_based_flush():
    """Verify that time-based accumulation uses the shared OUTPUT_DIR."""
    time_limit_ms = 200
    collector = TelemetryCollector(mode="time", limit=time_limit_ms)
    
    collector.handle_message("timed_data")
    time.sleep(0.3) # Exceed the limit
    collector.handle_message("trigger") # Flush
    
    found_files = glob.glob(os.path.join(OUTPUT_DIR, "raw_data_*.txt"))
    assert len(found_files) == 1

def test_correct_directory_naming():
    """
    Strict check: ensures the collector is actually using 
    the variable it claims to use.
    """
    collector = TelemetryCollector(mode="count", limit=1)
    collector.handle_message("checking_path")
    
    # Check if the folder exists and matches our constant
    assert os.path.isdir(OUTPUT_DIR)
    assert OUTPUT_DIR == "output_collector"