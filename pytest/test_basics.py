from RLTest import Env
import yaml


def getConnectionByEnv(env):
    conn = None
    if env.env == 'oss-cluster':
        conn = env.envRunner.getClusterConnection()
    else:
        conn = env.getConnection()
    return conn


class testBasic:
    def __init__(self):
        self.env = Env()
        self.env.broadcast('rs.refreshcluster')
        conn = getConnectionByEnv(self.env)
        for i in range(100):
            conn.execute_command('set', str(i), str(i))

    def testBasicQuery(self):
        self.env.expect('rs.pyexecute', "starCtx('test1', '*').map(lambda x:str(x)).collect().run()").ok()
        res = self.env.cmd('RS.getresultsblocking', 'test1')
        res = [yaml.load(r) for r in res]
        for i in range(100):
            self.env.assertContains({'value': str(i), 'key': str(i)}, res)

    def testBasicFilterQuery(self):
        self.env.expect('rs.pyexecute', 'starCtx("test2", "*").filter(lambda x: int(x["value"]) >= 50).map(lambda x:str(x)).collect().run()').ok()
        res = self.env.cmd('rs.getresultsblocking', 'test2')
        res = [yaml.load(r) for r in res]
        for i in range(50, 100):
            self.env.assertContains({'value': str(i), 'key': str(i)}, res)

    def testBasicMapQuery(self):
        self.env.expect('rs.pyexecute', 'starCtx("test3", "*").map(lambda x: x["value"]).map(lambda x:str(x)).collect().run()').ok()
        res = self.env.cmd('rs.getresultsblocking', 'test3')
        res = [yaml.load(r) for r in res]
        self.env.assertEqual(set(res), set([i for i in range(100)]))

    def testBasicGroupByQuery(self):
        self.env.expect('rs.pyexecute', 'starCtx("test4", "*").'
                                      'map(lambda x: {"key":x["key"], "value": 0 if int(x["value"]) < 50 else 100}).'
                                      'groupby(lambda x: str(x["value"]), lambda key, vals: len(vals)).'
                                      'map(lambda x:str(x)).collect().run()').ok()
        res = self.env.cmd('rs.getresultsblocking', 'test4')
        self.env.assertContains("{'value': 50, 'key': '100'}", res)
        self.env.assertContains("{'value': 50, 'key': '0'}", res)


def testFlatMap(env):
    env.broadcast('rs.refreshcluster')
    conn = getConnectionByEnv(env)
    conn.execute_command('lpush', 'l', '1', '2', '3')
    env.expect('rs.pyexecute', "starCtx('test', '*')."
                               "flatMap(lambda x: x['value'])."
                               "collect().run()").ok()
    res = env.cmd('rs.getresultsblocking', 'test')
    env.assertEqual(set(res), set(['1', '2', '3']))


def testLimit(env):
    env.broadcast('rs.refreshcluster')
    conn = getConnectionByEnv(env)
    conn.execute_command('lpush', 'l', '1', '2', '3')
    env.expect('rs.pyexecute', "starCtx('test', '*')."
                               "flatMap(lambda x: x['value'])."
                               "limit(1).collect().run()").ok()
    res = env.cmd('rs.getresultsblocking', 'test')
    env.assertEqual(len(res), 1)


def testRepartitionAndWriteOption(env):
    env.broadcast('rs.refreshcluster')
    conn = getConnectionByEnv(env)
    conn.execute_command('set', 'x', '1')
    conn.execute_command('set', 'y', '2')
    conn.execute_command('set', 'z', '3')
    env.expect('rs.pyexecute', "starCtx('test', '*')."
                               "repartition(lambda x: x['value'])."
                               "write(lambda x: redistar.saveKey(x['value'], x['key']))."
                               "map(lambda x : str(x)).collect().run()").ok()
    res = env.cmd('rs.getresultsblocking', 'test')
    env.assertContains("'value': '1'", str(res))
    env.assertContains("'key': 'x'", str(res))
    env.assertContains("'value': '2'", str(res))
    env.assertContains("'key': 'y'", str(res))
    env.assertContains("'value': '3'", str(res))
    env.assertContains("'key': 'z'", str(res))
    env.assertEqual(conn.execute_command('get', '1'), 'x')
    env.assertEqual(conn.execute_command('get', '2'), 'y')
    env.assertEqual(conn.execute_command('get', '3'), 'z')
