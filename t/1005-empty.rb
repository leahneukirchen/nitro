require './t/case'

with_fixture({}) do |svdir|
  testcase(svdir) { |events|
    sleep 1

    FileUtils.mkdir(File.join(svdir, "sv_a"))

    `nitroctl rescan`

    `nitroctl`.empty? or raise
  }
end
