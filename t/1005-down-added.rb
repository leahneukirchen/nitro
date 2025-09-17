require './t/case'

with_fixture({}) do |svdir|
  testcase(svdir) { |events|
    sleep 1
    
    FileUtils.mkdir(File.join(svdir, "sv_a"))
    File.write File.join(svdir, "sv_a/down"), ""
    File.open(File.join(svdir, "sv_a/run"), "w", 0755) { |f|
      f << <<EOF
#!/bin/sh
exec sleep 100
EOF
    }

    `nitroctl rescan`

    `nitroctl` =~ /DOWN sv_a/  or raise "ignored down"
  }
end
