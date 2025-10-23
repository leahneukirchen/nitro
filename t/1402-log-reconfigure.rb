require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/log=" => "../mylog", "mylog/run!" => <<EOF_B, "myotherlog/run!" => <<EOF_C do |svdir|
#!/bin/sh
echo 1
sleep 1
echo 2
echo 3
exec sleep 100
EOF_A
#!/bin/sh
exec cat >>mylog.txt
EOF_B
#!/bin/sh
exec cat >>myotherlog.txt
EOF_C
  testcase(svdir) { |events|
    events.poll_for(["STARTING", "sv_a"])
    events.poll_for(["UP", "mylog"])
    sleep 0.5

    !File.empty?(File.join(svdir, "mylog/mylog.txt"))  or raise "no log 1"
    File.empty?(File.join(svdir, "myotherlog/myotherlog.txt"))  or raise "got log 2"

    File.unlink File.join(svdir, "sv_a/log")
    File.symlink "../myotherlog", File.join(svdir, "sv_a/log")
    `nitroctl restart sv_a`
    events.poll_for(["UP", "sv_a"])

    !File.empty?(File.join(svdir, "myotherlog/myotherlog.txt"))  or raise "switch log failed"
  }
end
