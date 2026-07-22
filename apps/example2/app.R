library(tabler)

ui <- page(
  title = "Example 2",
  layout = "boxed",
  navbar = list(top = topbar(title = "Example 2")),
  body = body(
    p("A per-session counter. Each browser session gets its own R worker, so"),
    p("the count is independent from other sessions."),
    actionButton("inc", "Increment"),
    textOutput("count")
  )
)

server <- function(input, output, session) {
  n <- reactiveVal(0L)
  observeEvent(input$inc, {
    n(n() + 1L)
  })
  output$count <- renderText({
    paste("Count:", n())
  })
}

# debug, not for tabler-server running
# tablerApp(ui, server)
