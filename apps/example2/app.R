library(tabler)

# This app defines `ui` and `server` without calling tablerApp(); tabler-server
# launches it on an assigned port. Both styles are supported.

ui <- page(
  title = "Example 2",
  layout = "vertical",
  navbar = list(
    side = sidebar_menu(
      menu_item("Counter", icon = "adjustments")
    )
  ),
  body = body(
    h2("Example 2"),
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
