import os
import tempfile

# Before musemac (and the SDK's config) is imported: never touch the real state.
os.environ["MUSEGADGET_STATE_DIR"] = tempfile.mkdtemp(prefix="musemac-test-")
