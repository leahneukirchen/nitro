require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_b/run!" => <<EOF_B,
#!/bin/sh
exec sleep 100
EOF_A
#!/bin/sh
exec sleep 100
EOF_B
             "SYS/setup!" => <<EOF_SYS_SETUP, "SYS/finish!" => <<EOF_SYS_FINISH do |svdir|
#!/bin/sh
nitroctl start sv_a
EOF_SYS_SETUP
#!/bin/sh
echo $@ > finish_args
exec sleep 1
EOF_SYS_FINISH
  testcase(svdir) { |events|
    events.poll_for(["UP", "sv_b"])

    `nitroctl Reboot`
    sleep 1

    events.poll_for(["DOWN", "sv_a"])

    File.read(File.join(svdir, "SYS/finish_args")) == "0 0 reboot\n"  or raise "wrong finish_args"

    events.poll_for(["STARTING", "sv_a"])

    match_seq?(events, [["SETUP", "SYS"],
                        ["UP", "sv_a"],
                        ["DOWN", "SYS"],
                        ["SETUP", "SYS"],
                        ["STARTING", "sv_a"]])
  }
end
