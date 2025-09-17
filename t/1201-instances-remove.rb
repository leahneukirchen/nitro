require './t/case'

with_fixture "sv@/run!" => <<EOF_A, "sv@a=" => "sv@" do |svdir|
#!/bin/sh
exec sleep 100
EOF_A
  testcase(svdir) { |events|
    events.poll_for(["STARTING", "sv@a"])

    `rm -r #{svdir}/sv@`
    `rm #{svdir}/sv@a`
    `nitroctl rescan`
    events.poll_for(["DOWN", "sv@a"])

    `nitroctl`.empty?  or raise
  }
end

