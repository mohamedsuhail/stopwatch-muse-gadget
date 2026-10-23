"""This Mac as a Muse gadget: the SDK's Linux gadget, with CoreBluetooth for pairing.

Importing the package points the SDK at its checkout and at this Mac's state
directory (see :mod:`musemac.sdk`), so ``musegadget`` imports work after it.
"""

from musemac import sdk

sdk.use()

__version__ = "0.1.0"
