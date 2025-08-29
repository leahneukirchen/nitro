require './t/case'

with_fixture({}) do |svdir|
  testcase(svdir) { |events|
    sleep 1
    
    FileUtils.mkdir(File.join(svdir, "sv_a"))
    File.open(File.join(svdir, "sv_a/run"), "w", 0755) { |f|
      f << <<EOF
#!/bin/sh
sleep 100
EOF
    }

    `nitroctl rescan`
    # service is brought up due to rescan
    events.poll_for(["UP", "sv_a"])
    match_seq?(events, [["STARTING", "sv_a"], ["UP", "sv_a"]])
    
    p `nitroctl`
  }
end

