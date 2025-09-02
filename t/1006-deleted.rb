require './t/case'

with_fixture "sv_a/run!" => <<EOF_A do |svdir|
#!/bin/sh
sleep 100
EOF_A
  testcase(svdir) { |events|
    events.poll_for(["UP", "sv_a"])

    `nitroctl down sv_a`
    `rm -r #{svdir}/sv_a`
    `nitroctl up sv_a`

    `nitroctl` =~ /FATAL/  or raise
  }
end
