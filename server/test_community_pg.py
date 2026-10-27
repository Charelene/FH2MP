"""Run every community store contract on PostgreSQL and the real migrations.

The SQLite lane remains useful for fast checks, but cannot verify PostgreSQL
constraints, SQL translation, identity sequences or migration compatibility.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_community as contract
import test_db


class CommunityPostgres(contract.SocialTests):
    db_factory = staticmethod(test_db.fresh_db)
