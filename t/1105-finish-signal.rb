require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/finish!" => <<EOF_FINISH do |svdir|
#!/bin/sh
exec sleep 100
EOF_A
#!/bin/sh
echo $@ > finish_args
EOF_FINISH
  testcase(svdir) { |events|
    events.poll_for(["UP", "sv_a"])

    `nitroctl hup sv_a`
    $?.exitstatus == 0  or raise "nitroctl hup failed"
    events.poll_for(["DOWN", "sv_a"])

    p File.read(File.join(svdir, "sv_a/finish_args"))
    File.read(File.join(svdir, "sv_a/finish_args")) == "-1 1\n"  or raise "wrong finish_args"
  }
end

