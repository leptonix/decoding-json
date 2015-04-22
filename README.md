decoding_json
=============

This plugin receives the changes from WAL and
decodes them to JSON.

Adapted from postgresql-9.4.1/contrib/test_decoding

format
------
    {"type":"transaction.begin","xid":"2010539","committed":"2015-04-22 12:04:17.498163+00"}
    {"type":"table","name":"tv.kijkcijfers","change":"INSERT","data":{"id":1,"time":"2014-02-17 02:00:00+00","channel":1,"name":"journaal","kdh":0.4,"madl":14.4,"abs":58000}}
    {"type":"table","name":"tv.kijkcijfers","change":"UPDATE","data":{"id":1,"time":"2014-02-17 02:00:00+00","channel":1,"name":"test","kdh":0.4,"madl":14.4,"abs":58000}}
    {"type":"table","name":"tv.kijkcijfers","change":"DELETE","data":{"id":2}}
    {"type":"transaction.commit","xid":"2010539","committed":"2015-04-22 12:04:17.498163+00"}

install
-------
    sudo apt-get install postgresql-server-dev-9.4
    make
    sudo cp decoding_json.so /usr/lib/postgresql/9.4/lib/

configuration of postgresql
---------------------------
see <http://michael.otacoo.com/postgresql-2/postgres-9-4-feature-highlight-basics-logical-decoding/>
