require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/log=" => "../mylog", "mylog/run!" => <<EOF_B,
#!/bin/sh
exec sleep 100
EOF_A
#!/bin/sh
exec cat >mylog.txt
EOF_B
             "sv_c/run!" => "", "sv_c/down" => "" do |svdir|
  testcase(svdir) { |events|
    events.poll_for(["UP", "sv_a"])
    events.poll_for(["UP", "mylog"])

    pid_mylog = `nitroctl`[/mylog.*pid (\d+)/, 1]
    pid_sv_a = `nitroctl`[/sv_a.*pid (\d+)/, 1]
    pids = `nitroctl pidof sv_a mylog`
    pids == "#{pid_sv_a}\n#{pid_mylog}\n"  or raise "pidof mismatch"

    `nitroctl check mylog sv_a`
    $?.exitstatus == 0  or raise "check failed"

    `nitroctl pidof sv_c 2>&1`
    $?.exitstatus == 1  or raise "down pidof failed"

    `nitroctl check sv_c 2>&1`
    $?.exitstatus == 1  or raise "down check failed"
  }
end
