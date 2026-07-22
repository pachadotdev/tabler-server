library(tabler)

ui <- page(
  title = "Example 1",
  layout = "boxed",
  navbar = list(top = topbar(title = "Example 1")),
  body = body(
    p("Type your name and it is echoed back below."),
    textInput("name", "Your name", value = "world"),
    textOutput("greeting")
  )
)

server <- function(input, output, session) {
  output$greeting <- renderText({
    paste0("Hello, ", input$name, "!")
  })
}

# debug, not for tabler-server running
# tablerApp(ui, server)
