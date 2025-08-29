require './t/case'

with_fixture "sv_a/run!" => <<EOF_A do |svdir|
#!/bin/sh
sleep 100
EOF_A
  testcase(svdir) { |events|
    events.poll_for(["UP", "sv_a"])

    File.write File.join(svdir, "sv_a/down"), ""

    `nitroctl rescan`
    `nitroctl` =~ /UP sv_a/  or raise "went down"
  }
end

