assert  = require 'assert'
leveldb = require '../lib'





describe 'db-async', ->
  db = new leveldb.DB
  path = "#{__dirname}/../tmp/db-test-file"

  itShouldBehaveLikeAKeyValueStore = (key, val) ->

    it 'should open database', (done) ->
      db.open path, { create_if_missing: true, paranoid_checks: true }, done

    it 'should put key/value pair', (done) ->
      db.put key, val, done

    it 'should get key/value pair', (done) ->
      db.get key, (err, result) ->
        assert.ifError err
        assert.equal result, val.toString()
        done()

    it 'should delete key', (done) ->
      db.del key, done

    it 'should not get key/value pair', (done) ->
      db.get key, (err, result) ->
        assert.ifError err
        assert.equal result, undefined
        done()

    it 'should close database', (done) ->
      db.close done

  describe 'ascii', ->
    itShouldBehaveLikeAKeyValueStore "Hello", "World"

  describe 'buffer', ->
    key = new Buffer [1,9,9,9]
    val = new Buffer [1,2,3,4]
    itShouldBehaveLikeAKeyValueStore key, val
