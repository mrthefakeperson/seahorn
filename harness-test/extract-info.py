import re
import os
import os.path
import sqlite3
from sys import argv

file = open('files.txt', 'r')

x = file.readlines()

filecoderegex = re.compile('[0-9][0-9][0-9]?_[0-9][a-z]?')
linuxpathregex = re.compile('(drivers|sound|net|fs).*ko')
linuxpathregex2 = re.compile('(drivers|sound|net|fs).*?(?=--)')
linuxversionregex = re.compile('linux-[0-9](\.[0-9]+)?')
harnesstyperegex = re.compile('(main[0-9]*|m[0-9]*|cilled|entry_point.*)_false-unreach-call')

def classify(fname):
    fname = fname.strip()

    parts = fname.split('/')
    folder, file = parts[-2], parts[-1]
    fname = file

    path = "harnesses/%s/details/%s-details.txt" % (argv[1], fname)
    if not os.path.exists(path):
        return None
    details = open(path, 'r')
    lines = list(map(lambda e: e.strip(), details.readlines()))
    executionerror, timeout, harnesssuccess = False, False, None
    for line in lines:
        executionerror = executionerror or line == "RUNTIME ERROR"
        timeout = timeout or line == "TIME LIMIT EXCEEDED"
        harnesssuccess = harnesssuccess or line == "SUCCESSFUL HARNESS"
        harnesssuccess = line != "FAILED HARNESS" and harnesssuccess
    lines = list(filter(lambda e: e not in ["RUNTIME ERROR", "TIME LIMIT EXCEEDED"], lines))
    if "Command terminated by " in lines[0] or "Command exited with " in lines[0]:
        lines = lines[1:]
    if "could not link harness" in lines[0]:
        harnesssuccess = "FAILED LINK"
        runtime, memory, codefilesize, harnessfilesize = 0, 0, 0, 0
    else:
        runtime = float(lines[2]) + float(lines[4])
        memory = int(lines[6])
        codefilesize = int(lines[10])
        harnessfilesize = int(lines[12])

    details.close()
    
    filecode = filecoderegex.search(file)
    filecode = filecode.group(0) if filecode else 'unknown'
    
    linuxpath = linuxpathregex.search(file)
    if not linuxpath:
        linuxpath = linuxpathregex2.search(file)
        linuxpath = linuxpath.group(0).replace('-', '/') if linuxpath else 'unknown'
    elif '--' in linuxpath.group(0):
        linuxpath = linuxpath.group(0).replace('--', '/')
    else:
        linuxpath = linuxpath.group(0).replace('-', '/')
    linuxpath.replace('/ko', '.ko')
    
    linuxversion = linuxversionregex.search(file)
    linuxversion = linuxversion.group(0) if linuxversion else 'unknown'

    harnesstype = harnesstyperegex.search(file)
    harnesstype = harnesstype.group(0) if harnesstype else 'false-unreach-call'

    return {
        'file code': filecode,
        'linux path': linuxpath,
        'linux version': linuxversion,
        'harness type': harnesstype,
        'file name': fname,
        'runtime': runtime,
        'memory': memory,
        'code file size': codefilesize,
        'harness file size': harnessfilesize,
        'runtime error': executionerror,
        'timeout': timeout,
        'harness success': harnesssuccess
    }

if os.path.exists("harness-data.db"):
    os.remove("harness-data.db")
conn = sqlite3.connect("harness-data.db")
c = conn.cursor()
c.execute(
    '''create table data (
      "file code" text, "linux path" text, "linux version" text,
      "harness type" text, "file name" text,
      "runtime (s)" real, "memory (kB)" number, "code file size" number, "harness file size" number,
      "runtime error" text, "timeout" text, "harness success" text
      )'''
    )
for e in filter(lambda e: e, map(classify, x)):
    print("insert into data values (%s)" % ",".join(map(lambda ee: '"%s"' % str(e[ee]), e)))
    c.execute("insert into data values (%s)" % ",".join(map(lambda ee: '"%s"' % str(e[ee]), e)))

conn.commit()
conn.close()

file.close()
