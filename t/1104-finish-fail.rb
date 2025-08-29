require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/finish!" => <<EOF_FINISH do |svdir|
#!/bin/sh
sleep 100
EOF_A
#!/bin/sh
echo $@ > finish_args
exit 1
EOF_FINISH
  testcase(svdir) { |events|
    events.poll_for(["UP", "sv_a"])
    `nitroctl down sv_a`

    events.poll_for(["DOWN", "sv_a"])
    
    # failure of finish script is ignored
    match_seq?(events, [["STARTING", "sv_a"], ["UP", "sv_a"], ["SHUTDOWN", "sv_a"], ["DOWN", "sv_a"]])

    File.read(File.join(svdir, "sv_a/finish_args")) == "-1 15\n"  or raise "wrong finish_args"
  }
end

