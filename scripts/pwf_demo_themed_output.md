# pg_write_forward — themed demo run

> Captured transcript of `scripts/pwf_demo_themed.sh`.
> Colors are preserved via inline HTML — GitHub renders this correctly.
> Re-generate with: `scripts/pwf_demo_themed.sh --no-pause --tee scripts/pwf_demo_themed_output.txt && scripts/ansi_to_md.py scripts/pwf_demo_themed_output.txt scripts/pwf_demo_themed_output.md "pg_write_forward — themed demo run"`

<pre style="background-color:#1e1e1e;color:#cccccc;padding:14px;border-radius:6px;font-family:'SFMono-Regular',Consolas,'Liberation Mono',Menlo,monospace;font-size:12.5px;line-height:1.45;overflow-x:auto;">
Timing is on.

<span style="color:#2472c8">    ╔══════════════════════════════════════════════════════════════════╗ </span>
<span style="color:#2472c8">    ║ </span><span style="color:#ffffff;background-color:#2472c8">          📡   p g _ w r i t e _ f o r w a r d   📡                  </span> <span style="color:#2472c8"> ║ </span>
<span style="color:#2472c8">    ║ </span><span style="color:#11a8cd">          the standby that secretly forwards your writes            </span> <span style="color:#2472c8"> ║ </span>
<span style="color:#2472c8">    ╚══════════════════════════════════════════════════════════════════╝ </span>

<span style="opacity:0.65">        client ──INSERT──▶  </span> <span style="color:#e5e510"> 🛰  STANDBY </span> <span style="opacity:0.65">  ───relay──▶  </span> <span style="color:#0dbc79"> 🏛  PRIMARY </span>
<span style="opacity:0.65">                                   ▲                              │      </span>
<span style="opacity:0.65">                                   └──── streaming replication ◀──┘      </span>

<span style="color:#ffffff;background-color:#2472c8">  SCENE 0  </span> <span style="font-weight:bold">  Pre-flight check — are we really on the standby?  </span>
SELECT pg_is_in_recovery()                                  AS on_standby,
       current_setting(&#x27;shared_preload_libraries&#x27;)          AS preload,
       current_setting(&#x27;pg_write_forward.consistency&#x27;)      AS consistency_mode;
 on_standby |     preload      | consistency_mode 
------------+------------------+------------------
 t          | pg_write_forward | session
(1 row)

Time: 0.370 ms
<span style="color:#11a8cd"> ── current relay status ── </span>
SELECT * FROM pg_write_forward_status();
                                 primary_conninfo                                  | consistency | enabled | connected | forwarded_count | last_remote_lsn 
-----------------------------------------------------------------------------------+-------------+---------+-----------+-----------------+-----------------
 host=/home/azureuser/pg/pwf_demo/sockets port=55501 dbname=postgres user=postgres | session     | t       | f         |               0 | 
(1 row)

Time: 0.192 ms

<span style="color:#ffffff;background-color:#2472c8">  SCENE 1  </span> <span style="font-weight:bold">  Bootstrap schema on the primary (DDL is NOT forwarded)  </span>
<span style="opacity:0.65">    Why? DDL would need catalog locks the standby cannot replay. </span>
<span style="opacity:0.65">    We connect to the primary directly to set the stage. </span>
You are now connected to database &quot;postgres&quot; as user &quot;postgres&quot; via socket in &quot;/home/azureuser/pg/pwf_demo/sockets&quot; at port &quot;55501&quot;.
<span style="color:#0dbc79">    ✓ connected to primary </span>
DROP TABLE IF EXISTS pwf_demo;
DROP TABLE
Time: 3.483 ms
CREATE TABLE pwf_demo (id serial PRIMARY KEY, payload text);
CREATE TABLE
Time: 8.253 ms
INSERT INTO pwf_demo (payload) VALUES (&#x27;written-directly-on-primary&#x27;);
INSERT 0 1
Time: 4.049 ms
You are now connected to database &quot;postgres&quot; as user &quot;postgres&quot; via socket in &quot;/home/azureuser/pg/pwf_demo/sockets&quot; at port &quot;55502&quot;.
<span style="color:#e5e510">    ↩ back on the standby </span>
SELECT pg_sleep(0.3);
 pg_sleep 
----------
 
(1 row)

Time: 300.686 ms
                             Table &quot;public.pwf_demo&quot;
 Column  |  Type   | Collation | Nullable |               Default                
---------+---------+-----------+----------+--------------------------------------
 id      | integer |           | not null | nextval(&#x27;pwf_demo_id_seq&#x27;::regclass)
 payload | text    |           |          | 
Indexes:
    &quot;pwf_demo_pkey&quot; PRIMARY KEY, btree (id)


<span style="color:#ffffff;background-color:#2472c8">  SCENE 2  </span> <span style="font-weight:bold">  INSERT with forwarding OFF — expect a wall  </span>
<span style="opacity:0.65">    The standby is read-only by default; INSERT must fail. </span>
SET pg_write_forward.enabled = off;
SET
Time: 0.077 ms
INSERT INTO pwf_demo (payload) VALUES (&#x27;this should fail&#x27;);
psql:scripts/pwf_demo_themed.sql:71: ERROR:  cannot execute INSERT in a read-only transaction
Time: 0.108 ms
<span style="color:#cd3131">    🚧 expected: cannot execute INSERT in a read-only transaction </span>

<span style="color:#ffffff;background-color:#2472c8">  SCENE 3  </span> <span style="font-weight:bold">  Flip the relay ON and INSERT from the standby  </span>
SET pg_write_forward.enabled = on;
SET
Time: 0.057 ms
INSERT INTO pwf_demo (payload) VALUES (&#x27;hello-from-standby&#x27;);
INSERT 0 1
Time: 7.726 ms
<span style="color:#0dbc79">    📡 INSERT relayed primary→standby; row should now be visible: </span>
SELECT count(*) AS rows_visible_on_standby FROM pwf_demo;
 rows_visible_on_standby 
-------------------------
                       2
(1 row)

Time: 0.199 ms

<span style="color:#ffffff;background-color:#2472c8">  SCENE 4  </span> <span style="font-weight:bold">  UPDATE, DELETE, MERGE — every write hops to the primary  </span>
UPDATE pwf_demo SET payload = upper(payload) WHERE id = 1;
UPDATE 1
Time: 3.505 ms
DELETE FROM pwf_demo WHERE payload LIKE &#x27;%-locked%&#x27;;
DELETE 0
Time: 0.296 ms
You are now connected to database &quot;postgres&quot; as user &quot;postgres&quot; via socket in &quot;/home/azureuser/pg/pwf_demo/sockets&quot; at port &quot;55501&quot;.
SELECT setval(pg_get_serial_sequence(&#x27;pwf_demo&#x27;,&#x27;id&#x27;),
              GREATEST((SELECT max(id) FROM pwf_demo), 100));
 setval 
--------
    100
(1 row)

Time: 2.413 ms
You are now connected to database &quot;postgres&quot; as user &quot;postgres&quot; via socket in &quot;/home/azureuser/pg/pwf_demo/sockets&quot; at port &quot;55502&quot;.
SET pg_write_forward.enabled = on;
SET
Time: 0.120 ms
MERGE INTO pwf_demo d
USING (VALUES (&#x27;merged-row-locked&#x27;), (&#x27;another-merged&#x27;)) AS s(p)
   ON false
 WHEN NOT MATCHED THEN INSERT (payload) VALUES (s.p);
MERGE 2
Time: 7.938 ms
SELECT id, payload FROM pwf_demo ORDER BY id;
 id  |           payload           
-----+-----------------------------
   1 | WRITTEN-DIRECTLY-ON-PRIMARY
   2 | hello-from-standby
 101 | merged-row-locked
 102 | another-merged
(4 rows)

Time: 0.323 ms

<span style="color:#ffffff;background-color:#2472c8">  SCENE 5  </span> <span style="font-weight:bold">  SELECT … FOR UPDATE — lock taken on the primary  </span>
BEGIN;
BEGIN
Time: 0.045 ms
SELECT id, payload FROM pwf_demo WHERE id = 1 FOR UPDATE;
 id |           payload           
----+-----------------------------
  1 | WRITTEN-DIRECTLY-ON-PRIMARY
(1 row)

Time: 0.332 ms
<span style="color:#bc3fbc">    🔒 row locked on primary; lock released on COMMIT </span>
COMMIT;
COMMIT
Time: 3.228 ms

<span style="color:#ffffff;background-color:#2472c8">  SCENE 6  </span> <span style="font-weight:bold">  Prepared writes relay just like ad-hoc ones  </span>
PREPARE ins (text) AS INSERT INTO pwf_demo (payload) VALUES ($1);
PREPARE
Time: 0.174 ms
EXECUTE ins(&#x27;via-prepared-1&#x27;);
EXECUTE
Time: 3.978 ms
EXECUTE ins(&#x27;via-prepared-2&#x27;);
EXECUTE
Time: 3.568 ms

<span style="color:#ffffff;background-color:#2472c8">  SCENE 7  </span> <span style="font-weight:bold">  EXPLAIN ANALYZE INSERT — timings come from the primary  </span>
EXPLAIN (ANALYZE, BUFFERS, VERBOSE)
INSERT INTO pwf_demo (payload) VALUES (&#x27;explained&#x27;);
                                                QUERY PLAN                                                
----------------------------------------------------------------------------------------------------------
 Insert on public.pwf_demo  (cost=0.00..0.01 rows=0 width=0) (actual time=0.012..0.012 rows=0.00 loops=1)
   Buffers: shared hit=3
   -&gt;  Result  (cost=0.00..0.01 rows=1 width=36) (actual time=0.002..0.002 rows=1.00 loops=1)
         Output: nextval(&#x27;pwf_demo_id_seq&#x27;::regclass), &#x27;explained&#x27;::text
         Buffers: shared hit=1
 Planning Time: 0.009 ms
 Execution Time: 0.017 ms
(7 rows)

Time: 3.251 ms

<span style="color:#ffffff;background-color:#2472c8">  SCENE 8  </span> <span style="font-weight:bold">  Multi-statement transaction sees its own writes  </span>
BEGIN;
BEGIN
Time: 0.036 ms
INSERT INTO pwf_demo (payload) VALUES (&#x27;xact-row-A&#x27;);
INSERT 0 1
Time: 0.191 ms
INSERT INTO pwf_demo (payload) VALUES (&#x27;xact-row-B&#x27;);
INSERT 0 1
Time: 0.099 ms
SELECT id, payload FROM pwf_demo
 WHERE payload LIKE &#x27;xact-row-%&#x27; ORDER BY id;
 id  |  payload   
-----+------------
 106 | xact-row-A
 107 | xact-row-B
(2 rows)

Time: 0.341 ms
COMMIT;
COMMIT
Time: 3.095 ms

<span style="color:#ffffff;background-color:#2472c8">  SCENE 9  </span> <span style="font-weight:bold">  search_path / application_name / TZ mirror to primary  </span>
SET application_name = &#x27;pwf-demo-mirrored&#x27;;
SET
Time: 0.092 ms
SET TIME ZONE &#x27;Asia/Tokyo&#x27;;
SET
Time: 0.306 ms
INSERT INTO pwf_demo (payload)
VALUES (&#x27;current_setting(application_name)=&#x27;
        || current_setting(&#x27;application_name&#x27;));
INSERT 0 1
Time: 3.320 ms
RESET TIME ZONE;
RESET
Time: 0.081 ms

<span style="color:#ffffff;background-color:#2472c8">  SCENE 10  </span> <span style="font-weight:bold">  GLOBAL: the standby waits for replay to catch up  </span>
SET pg_write_forward.consistency = global;
SET
Time: 0.090 ms
<span style="color:#11a8cd">    primary writes a row directly; standby must see it after replay </span>
You are now connected to database &quot;postgres&quot; as user &quot;postgres&quot; via socket in &quot;/home/azureuser/pg/pwf_demo/sockets&quot; at port &quot;55501&quot;.
INSERT INTO pwf_demo (payload) VALUES (&#x27;written-directly-on-primary-2&#x27;);
INSERT 0 1
Time: 1.971 ms
You are now connected to database &quot;postgres&quot; as user &quot;postgres&quot; via socket in &quot;/home/azureuser/pg/pwf_demo/sockets&quot; at port &quot;55502&quot;.
SET pg_write_forward.consistency = global;
SET
Time: 0.123 ms
SET pg_write_forward.enabled    = on;
SET
Time: 0.042 ms
INSERT INTO pwf_demo (payload) VALUES (&#x27;global-mode-probe&#x27;);
INSERT 0 1
Time: 5.631 ms
SELECT count(*) FILTER (WHERE payload LIKE &#x27;written-directly-on-primary%&#x27;)
       AS sees_primary_writes
  FROM pwf_demo;
 sees_primary_writes 
---------------------
                   1
(1 row)

Time: 0.430 ms

<span style="color:#ffffff;background-color:#2472c8">  SCENE 11  </span> <span style="font-weight:bold">  Inspect the relay station  </span>
SELECT * FROM pg_write_forward_status();
                                 primary_conninfo                                  | consistency | enabled | connected | forwarded_count | last_remote_lsn 
-----------------------------------------------------------------------------------+-------------+---------+-----------+-----------------+-----------------
 host=/home/azureuser/pg/pwf_demo/sockets port=55501 dbname=postgres user=postgres | global      | t       | t         |               1 | 0/03213F40
(1 row)

Time: 0.141 ms

<span style="color:#ffffff;background-color:#2472c8">  SCENE 12  </span> <span style="font-weight:bold">  Drop the relay link and bring it back  </span>
SELECT pg_write_forward_disconnect();
 pg_write_forward_disconnect 
-----------------------------
 
(1 row)

Time: 0.102 ms
SELECT * FROM pg_write_forward_status();
                                 primary_conninfo                                  | consistency | enabled | connected | forwarded_count | last_remote_lsn 
-----------------------------------------------------------------------------------+-------------+---------+-----------+-----------------+-----------------
 host=/home/azureuser/pg/pwf_demo/sockets port=55501 dbname=postgres user=postgres | global      | t       | f         |               1 | 0/03213F40
(1 row)

Time: 0.064 ms
INSERT INTO pwf_demo (payload) VALUES (&#x27;after-reconnect&#x27;);
INSERT 0 1
Time: 5.289 ms
SELECT * FROM pg_write_forward_status();
                                 primary_conninfo                                  | consistency | enabled | connected | forwarded_count | last_remote_lsn 
-----------------------------------------------------------------------------------+-------------+---------+-----------+-----------------+-----------------
 host=/home/azureuser/pg/pwf_demo/sockets port=55501 dbname=postgres user=postgres | global      | t       | t         |               2 | 0/03213FF8
(1 row)

Time: 0.185 ms

<span style="color:#000000;background-color:#0dbc79">  CURTAIN  </span> <span style="font-weight:bold">  Every row below was written from the standby (or globally)  </span>
SELECT id, payload FROM pwf_demo ORDER BY id;
 id  |                       payload                       
-----+-----------------------------------------------------
   1 | WRITTEN-DIRECTLY-ON-PRIMARY
   2 | hello-from-standby
 101 | merged-row-locked
 102 | another-merged
 103 | via-prepared-1
 104 | via-prepared-2
 105 | explained
 106 | xact-row-A
 107 | xact-row-B
 108 | current_setting(application_name)=pwf-demo-mirrored
 109 | written-directly-on-primary-2
 110 | global-mode-probe
 111 | after-reconnect
(13 rows)

Time: 0.261 ms

<span style="color:#0dbc79">    ✅  demo complete — relay station signing off </span>
<span style="opacity:0.65">    tip: re-run with --no-pause for a fast replay, </span>
<span style="opacity:0.65">         or --tee out.log to capture a transcript. </span>

</pre>
