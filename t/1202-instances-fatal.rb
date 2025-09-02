require './t/case'

with_fixture "sv@/run!" => <<EOF_A do |svdir|
#!/bin/sh
sleep 100
EOF_A
  testcase(svdir) { |events|
    sleep 1
    `nitroctl up sv@a`
    events.poll_for(["UP", "sv@a"])

    `nitroctl down sv@a`
    events.poll_for(["DOWN", "sv@a"])

    `rm -r #{svdir}/sv@`
    `nitroctl up sv@a 2>&1`

    `nitroctl` =~ /FATAL sv@a/  or raise

    `nitroctl rescan`

    `nitroctl` =~ /FATAL sv@a/  and raise "no cleanup"
  }
end
