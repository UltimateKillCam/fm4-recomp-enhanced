"""Native VIP compatibility change in a separate prepared copy. Modified 2026-10-23."""
from pathlib import Path
import hashlib,shutil,sqlite3
from resource_codec import Invalid
from overlay_plan import safe_absolute,overlap

def digest(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as stream:
        while data:=stream.read(1<<20):h.update(data)
    return h.hexdigest()

def vip_schema(stock):
    connection=sqlite3.connect(Path(stock).resolve().as_uri()+'?mode=ro',uri=True)
    try:
        row=connection.execute("SELECT sql FROM sqlite_master WHERE type='table' AND name='VipDiscounts'").fetchone()
        if row is None or connection.execute('SELECT COUNT(*) FROM VipDiscounts').fetchone()!=(0,):
            raise Invalid('Original native empty VipDiscounts proof does not match')
        # Exact original SQL pinned from the actual native database, including
        # whitespace. Do not accept arbitrary executable SQL from another DB.
        expected='CREATE TABLE [VipDiscounts]  (\n[Original] INTEGER PRIMARY KEY \n,[Discounted] BIGINT\n)'
        if row[0]!=expected:raise Invalid('Original native VipDiscounts schema differs')
        return row[0]
    finally:connection.close()

def create_prepared_copy(source,prepared,stock):
    source=safe_absolute(source,Path.cwd());prepared=safe_absolute(prepared,Path.cwd());stock=safe_absolute(stock,Path.cwd())
    if overlap(prepared,source) or overlap(prepared,stock):raise Invalid('Prepared database overlaps read-only input')
    if prepared.exists():raise Invalid('Prepared database already exists')
    prepared.parent.mkdir(parents=True,exist_ok=True)
    sql=vip_schema(stock);before=digest(source);stock_hash=digest(stock)
    shutil.copyfile(source,prepared)
    if digest(source)!=before or digest(prepared)!=before:raise Invalid('Database input changed during copy')
    connection=sqlite3.connect(prepared)
    try:
        if connection.execute('PRAGMA integrity_check').fetchall()!=[('ok',)]:raise Invalid('Source database integrity failed')
        old_tables=connection.execute("SELECT name,sql FROM sqlite_master WHERE type='table' ORDER BY name").fetchall()
        exists=connection.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='VipDiscounts'").fetchone() is not None
        if not exists:
            with connection:connection.execute(sql)
        else:
            # A supplied update may have a genuine table; preserve its rows and
            # semantics, while requiring the native query columns to exist.
            connection.execute('SELECT Original,Discounted FROM VipDiscounts LIMIT 0')
        if connection.execute('PRAGMA integrity_check').fetchall()!=[('ok',)]:raise Invalid('Prepared database integrity failed')
        new_tables=connection.execute("SELECT name,sql FROM sqlite_master WHERE type='table' ORDER BY name").fetchall()
        expected=sorted(old_tables+([] if exists else [('VipDiscounts',sql)]))
        if new_tables!=expected:raise Invalid('Unexpected prepared schema mutation')
    finally:connection.close()
    if digest(source)!=before or digest(stock)!=stock_hash:raise Invalid('Read-only database input changed')
    return {'policy':'native-vip-empty-schema-v1','applied':not exists,
        'source_database_sha256':before,'prepared_database_sha256':digest(prepared),
        'native_stock_database_sha256':stock_hash,'exact_stock_sql':sql,
        'sql_sha256':hashlib.sha256(sql.encode()).hexdigest(),'integrity_check':'ok',
        'car_pi_overrides_policy':'preserve supplied absence/content; no stock rows injected',
        'proof':'optional-table-review.md conditional native query null dereference trace'}
