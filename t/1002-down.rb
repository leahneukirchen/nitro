require './t/case'

with_fixture "sv_a/run!" => <<EOF_A, "sv_a/down" => "" do |svdir|
#!/bin/sh
sleep 100
EOF_A
  testcase(svdir) { |events|
    sleep 1
    # service is down due to down file
    `nitroctl` =~ /DOWN sv_a/  or raise "not down"

    # service can be brought up
    `nitroctl up sv_a`
    events.poll_for(["UP", "sv_a"])
    match_seq?(events, [["STARTING", "sv_a"], ["UP", "sv_a"]])

    `nitroctl rescan`
    sleep 1
    `nitroctl` =~ /UP sv_a/  or raise "not staying up"

    FileUtils.rm File.join(svdir, "sv_a/down")
    `nitroctl rescan`
    sleep 1
    `nitroctl` =~ /UP sv_a/  or raise "not staying up"
  }
end

